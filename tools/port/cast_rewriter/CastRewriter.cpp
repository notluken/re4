// re4_cast_rewriter -- Phase 2 build-time cast rewriter (docs/port-phase2.md section 8).
//
// Rewrites direct pointer<->integer casts (CK_PointerToIntegral / CK_IntegralToPointer) that are
// NOT inside a macro expansion into re4_port::GC32()/re4_port::GCPTR<T>() calls, for one
// translation unit at a time, and writes the rewritten text to an output file with a leading
// `#line 1 "<original path>"` so every diagnostic/debugger session still reports the vendor's
// real file. The original source tree is never modified; this only ever writes to its own
// -o output path (wired into CMake to land under build-pc/gen/).
//
// Sites this tool deliberately leaves alone (reported, not guessed):
//   - casts inside a macro expansion (ARC_PTR/FlagChk/... family; header-macro fix is a separate,
//     hand-verified step per docs/port-phase2.md section 6, not this tool's job).
//   - CK_PointerToIntegral casts whose destination integer type is wider than 32 bits (ambiguous:
//     could be identity/hash use, not a GC-address site -- flagged, left as the original cast).
//   - any cast whose pointee/argument type this tool cannot print as compilable C++ (flagged).
//
// Build: see tools/port/cast_rewriter/CMakeLists.txt (needs libTooling; Apple's bundled clang does
// not ship libTooling headers/libs, so this is built against Homebrew LLVM -- see docs/port-phase2.md).

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory ToolCategory("re4-cast-rewriter options");
llvm::cl::opt<std::string> OutputPath("o", llvm::cl::desc("Rewritten output file"),
                                       llvm::cl::cat(ToolCategory));
llvm::cl::opt<std::string> ReportPath("report", llvm::cl::desc("Ambiguous/skipped-site report (appended)"),
                                       llvm::cl::cat(ToolCategory));
llvm::cl::opt<std::string> ResourceDir(
    "resource-dir",
    llvm::cl::desc("clang resource dir to append (needed since this tool is not the clang driver "
                    "binary itself, so it cannot auto-detect one next to argv[0])"),
    llvm::cl::cat(ToolCategory));

// One line per rewritten or flagged site, appended to ReportPath if given, otherwise to stderr.
struct SiteLog {
    std::ofstream Out;
    bool ToStderr = true;
    void open(const std::string &path)
    {
        if (path.empty())
            return;
        Out.open(path, std::ios::app);
        ToStderr = false;
    }
    void log(const std::string &line)
    {
        if (ToStderr)
            llvm::errs() << line << "\n";
        else
            Out << line << "\n";
    }
} SLog;

class CastVisitor : public RecursiveASTVisitor<CastVisitor> {
public:
    CastVisitor(Rewriter &R, ASTContext &Ctx) : TheRewriter(R), Context(Ctx) {}

    bool VisitCStyleCastExpr(CStyleCastExpr *E) { return handleCast(E); }
    bool VisitCXXStaticCastExpr(CXXStaticCastExpr *E) { return handleCast(E); }
    bool VisitCXXReinterpretCastExpr(CXXReinterpretCastExpr *E) { return handleCast(E); }
    bool VisitImplicitCastExpr(ImplicitCastExpr *) { return true; } // implicit casts: never rewritten

private:
    Rewriter &TheRewriter;
    ASTContext &Context;
    // Nested pointer<->integer casts (e.g. `(T*)(u32) p`) are visited parent-before-child by
    // RecursiveASTVisitor's default pre-order traversal; once the outer cast's whole range has been
    // replaced, the inner cast's range is now inside already-rewritten text and a second
    // Rewriter::ReplaceText on it corrupts the buffer (measured: "expected ')'" downstream --
    // docs/port-phase2.md section 8's "cast result feeds ... more than one cast" case). Track
    // already-rewritten ranges and skip anything nested inside one; getSourceText() already pulls
    // the inner cast's *original* text into the outer replacement, so the composition still happens,
    // just via plain text substitution instead of a second AST-level rewrite.
    std::vector<std::pair<unsigned, unsigned>> RewrittenOffsets;

    bool isNestedInRewritten(SourceRange R)
    {
        const SourceManager &SM = Context.getSourceManager();
        unsigned Begin = SM.getFileOffset(SM.getSpellingLoc(R.getBegin()));
        unsigned End = SM.getFileOffset(SM.getSpellingLoc(R.getEnd()));
        for (auto &P : RewrittenOffsets) {
            if (Begin >= P.first && End <= P.second)
                return true;
        }
        return false;
    }

    static std::string siteLoc(const SourceManager &SM, SourceLocation Loc)
    {
        PresumedLoc P = SM.getPresumedLoc(Loc);
        if (P.isInvalid())
            return "<invalid>";
        return std::string(P.getFilename()) + ":" + std::to_string(P.getLine());
    }

    // Composes a qualifying cast's GC32()/GCPTR<T>() text without touching the Rewriter buffer --
    // used both for a cast visited directly (handleCast, which then does the one real edit for the
    // outermost cast in a chain) and, recursively, for a subexpression that is itself another
    // qualifying cast (e.g. RAW_U32(p, ofs)'s `(u32*) ((u32) (p) + (ofs))`: the outer
    // IntegralToPointer's subexpression contains a nested PointerToIntegral on `p`). Composing here
    // -- instead of relying on a second, separate Rewriter edit for the inner cast -- sidesteps
    // traversal-order/buffer-corruption entirely (docs/port-phase2.md section 8's "cast result feeds
    // ... more than one cast" case) and keeps `on error, leave the original text` correct: if the
    // inner cast is ambiguous (wide dest / unprintable pointee), this returns empty and the caller
    // falls back to the inner cast's own original source text, verbatim, inside the outer's
    // replacement -- exactly what independent-site rewriting would have left behind anyway.
    // `Reason`, if non-null, is filled with a FLAG/SKIP category string on failure so the top-level
    // caller (handleCast) can log it; recursive (composing) calls pass nullptr to stay silent, since
    // the RecursiveASTVisitor will visit that inner node on its own later and log for itself there.
    std::string tryBuildCastReplacement(ExplicitCastExpr *E, bool UseSpelling, std::string *Reason)
    {
        CastKind CK = E->getCastKind();
        if (CK != CK_PointerToIntegral && CK != CK_IntegralToPointer) {
            if (Reason)
                *Reason = "not-a-target-cast-kind";
            return {};
        }
        std::string SubText = buildSubexprText(E->getSubExpr(), UseSpelling);
        if (SubText.empty()) {
            if (Reason)
                *Reason = "unprintable-subexpr";
            return {};
        }
        if (CK == CK_PointerToIntegral) {
            std::string DestSpelling = E->getTypeAsWritten().getAsString();
            static const char *WideSpellings[] = {"u64", "s64", "uintptr_t", "size_t",
                                                    "long long", "unsigned long long"};
            for (const char *W : WideSpellings) {
                if (DestSpelling.find(W) != std::string::npos) {
                    if (Reason)
                        *Reason = "wide-dest(" + DestSpelling + ")";
                    return {};
                }
            }
            std::string Replacement = "re4_port::GC32(" + SubText + ")";
            if (!DestSpelling.empty())
                Replacement = "(" + DestSpelling + ")" + Replacement;
            return Replacement;
        }
        // CK_IntegralToPointer
        QualType DestTy = E->getType();
        if (!DestTy->isPointerType()) {
            if (Reason)
                *Reason = "non-pointer-dest";
            return {};
        }
        QualType Pointee = DestTy->getPointeeType();
        std::string PointeeStr = Pointee.getAsString();
        if (PointeeStr.empty() || PointeeStr.find("unnamed") != std::string::npos ||
            Pointee->isArrayType() || Pointee->isFunctionType()) {
            if (Reason)
                *Reason = "unprintable-pointee";
            return {};
        }
        return "re4_port::GCPTR<" + PointeeStr + ">((std::uint32_t)(" + SubText + "))";
    }

    struct CastSite {
        unsigned Begin, End; // file offsets (spelling-resolved when UseSpelling)
        std::string Replacement;
    };

    // Finds every *topmost* qualifying pointer<->integer cast under S (not just a direct child --
    // e.g. RAW_U32's `(u32*) ((u32) (p) + (ofs))` has the inner `(u32) (p)` cast as an operand of a
    // `+`, not the immediate subexpression of the outer cast). Does not recurse into a cast once
    // found qualifying: tryBuildCastReplacement already recurses into *its* own subexpression via
    // composeText, so any further nesting is handled there, not by double-visiting here.
    void collectNestedCasts(Stmt *S, bool UseSpelling, std::vector<CastSite> &Out)
    {
        if (!S)
            return;
        if (auto *CE = dyn_cast<ExplicitCastExpr>(S)) {
            // A macro-produced nested cast (its own ExprLoc is a macro ID) is already handled by
            // its own independent RecursiveASTVisitor visit to handleCast -- either SKIPped
            // (macro-in-header) or rewritten in place at the #define's own text (MacroBody, a macro
            // defined in this TU's own file). Composing it a second time *here*, inline at this
            // call site, double-processes it: measured on src/game/dvd.cpp's `(u32) DVD_BUFF2`
            // (`#define DVD_BUFF2 ((void*) 0x80360000)`, defined in dvd.cpp itself) -- the literal
            // `0x80360000` a level down is itself macro-produced, and composing it here produced
            // text mixing the call site's macro *name* with the definition's internal cast, wrong
            // either way E->getSubExpr() and the get-fixed macro body were meant to agree. Leave it
            // alone; buildSubexprText's caller falls back to getSourceText, which returns the
            // macro invocation's own call-site text unchanged (still correct C++ once the macro's
            // own body has been separately rewritten, if it needed to be).
            // Only skip when we are not already inside a macro-body composition (UseSpelling):
            // when UseSpelling is true, this nested cast's macro-ness is the SAME enclosing macro
            // whose body text we are currently splicing together (e.g. DATA_PTR's outer `(void*)
            // (... + (u32) (d))`, mercenaries.cpp -- the inner `(u32) (d)` is part of that same
            // #define and must still be spliced in, since its own independent handleCast visit
            // gets SKIPped as "already rewritten" -- nested inside the outer's just-recorded
            // range). When UseSpelling is false, a macro-produced nested cast is always a
            // *different*, independently-handled site (see the comment above).
            if (!UseSpelling && CE->getExprLoc().isMacroID())
                return;
            std::string Repl = tryBuildCastReplacement(CE, UseSpelling, nullptr);
            if (!Repl.empty()) {
                SourceManager &SM = Context.getSourceManager();
                SourceRange SR = CE->getSourceRange();
                SourceLocation B = SR.getBegin();
                SourceLocation En = SR.getEnd();
                if (UseSpelling) {
                    B = SM.getSpellingLoc(B);
                    En = SM.getSpellingLoc(En);
                }
                unsigned Bo = SM.getFileOffset(B);
                SourceLocation EndTok =
                    Lexer::getLocForEndOfToken(En, 0, SM, Context.getLangOpts());
                unsigned Eo = SM.getFileOffset(EndTok);
                Out.push_back({Bo, Eo, Repl});
                return; // do not look for further sites inside an already-composed cast
            }
        }
        for (Stmt *Child : S->children())
            collectNestedCasts(Child, UseSpelling, Out);
    }

    // getSourceText for a subexpression, except every qualifying pointer<->integer cast found
    // anywhere inside it (not just at the top) is composed in place via text splicing, back to
    // front by offset, instead of copied as raw (still-narrowing) text. See tryBuildCastReplacement
    // and collectNestedCasts for the two halves of this.
    std::string buildSubexprText(Expr *E, bool UseSpelling)
    {
        Expr *Stripped = E->IgnoreParens();
        if (auto *CE = dyn_cast<ExplicitCastExpr>(Stripped)) {
            // Same reasoning as collectNestedCasts's guard just below: a macro-produced cast that
            // is NOT part of the macro body we are already composing (UseSpelling) is handled by
            // its own independent handleCast visit, not composed again here.
            if (UseSpelling || !CE->getExprLoc().isMacroID()) {
                std::string Composed = tryBuildCastReplacement(CE, UseSpelling, nullptr);
                if (!Composed.empty())
                    return Composed;
            }
        }
        std::string Base = getSourceText(E, UseSpelling);
        if (Base.empty())
            return Base;
        std::vector<CastSite> Sites;
        collectNestedCasts(E, UseSpelling, Sites);
        if (Sites.empty())
            return Base;

        SourceManager &SM = Context.getSourceManager();
        SourceLocation EBegin = E->getSourceRange().getBegin();
        if (UseSpelling)
            EBegin = SM.getSpellingLoc(EBegin);
        unsigned BaseOffset = SM.getFileOffset(EBegin);

        std::sort(Sites.begin(), Sites.end(),
                  [](const CastSite &A, const CastSite &B) { return A.Begin > B.Begin; });
        for (const CastSite &Site : Sites) {
            if (Site.Begin < BaseOffset || Site.End < Site.Begin)
                continue; // defensive: a malformed/absent offset, leave Base untouched at this site
            unsigned RelB = Site.Begin - BaseOffset;
            unsigned RelE = Site.End - BaseOffset;
            if (RelE > Base.size() || RelB > RelE)
                continue;
            Base = Base.substr(0, RelB) + Site.Replacement + Base.substr(RelE);
        }
        return Base;
    }

    bool handleCast(ExplicitCastExpr *E)
    {
        CastKind CK = E->getCastKind();
        if (CK != CK_PointerToIntegral && CK != CK_IntegralToPointer)
            return true;

        const SourceManager &SM = Context.getSourceManager();
        SourceLocation Loc = E->getExprLoc();

        // Macro-expanded casts. Two cases, per the coordinator's split:
        //  - the macro is #define'd in THIS TU's own .cpp/.c file (its spelling location is in the
        //    main file): rewrite the #define BODY once (not per expansion site) in the generated
        //    copy -- the original tree's #define text is never touched, only build-pc/gen/'s.
        //  - the macro is #define'd in a header (spelling location elsewhere): left alone entirely,
        //    that is the hand-edited TARGET_PC-branch path (section 6), with real-file review and
        //    remote verification, not this tool's job.
        bool MacroBody = false;
        SourceLocation EditBegin = E->getSourceRange().getBegin();
        SourceLocation EditEnd = E->getSourceRange().getEnd();
        if (Loc.isMacroID()) {
            SourceLocation SpellLoc = SM.getSpellingLoc(Loc);
            if (!SM.isWrittenInMainFile(SpellLoc)) {
                SLog.log("SKIP macro-in-header " + siteLoc(SM, SpellLoc));
                return true;
            }
            MacroBody = true;
            EditBegin = SM.getSpellingLoc(EditBegin);
            EditEnd = SM.getSpellingLoc(EditEnd);
        } else if (!SM.isWrittenInMainFile(Loc)) {
            return true; // only rewrite casts physically in this TU's own file, not #included ones
        } else {
            // Not itself macro-bodied, but the cast's own begin/end token can still be a macro ID --
            // its *argument* (or, one level further, its subexpression) can be another macro call,
            // object-like (`(u32) DVD_BUFF2`, `#define DVD_BUFF2 ((void*) 0x80360000)`) or
            // function-like with its own nested expansions (`(u32) WEP_ARC_PTR(no)`, which expands
            // through PL_ARC_PTR's body). E->getSourceRange() then reports a macro-ID end location.
            // Using getSpellingLoc there would jump to the macro's own #define text (wrong file
            // position entirely -- measured, produced a corrupted double-text edit).
            // getExpansionLoc(Loc) always collapses to the *start* of the expansion that produced
            // Loc -- SourceManager's own doc: "the expansion location referenced by the ID" resolves
            // through ExpansionLocStart regardless of whether Loc was itself a begin or an end token
            // -- so using it for EditEnd truncates the replaced range to the macro invocation's
            // opening token (measured: `(u32) WEP_ARC_PTR(0x7)` rewritten to
            // `(u32)re4_port::GC32(...)(0x7)`, the trailing `(0x7)` left over because EditEnd landed
            // right after the macro name instead of after the closing `)`, with `arc`/`no` verbatim
            // from the macro body's spelling text since MacroBody was false so buildSubexprText's
            // getSourceText() spelling-fallback picked up the #define's own text, not the call
            // site's substituted arguments). getExpansionRange(Loc).getEnd() instead reports "the
            // range of tokens covered by the expansion in the ultimate file", i.e. the real end of
            // the whole macro invocation at the call site -- exactly what Rewriter::ReplaceText
            // needs. Begin already worked with getExpansionLoc precisely because a begin location's
            // expansion-start *is* the correct answer; kept as-is for clarity, but pinned through
            // the same ultimate-file-range accessor for symmetry.
            if (EditBegin.isMacroID())
                EditBegin = SM.getExpansionRange(EditBegin).getBegin();
            if (EditEnd.isMacroID())
                EditEnd = SM.getExpansionRange(EditEnd).getEnd();
        }

        SourceRange EditRange(EditBegin, EditEnd);
        if (isNestedInRewritten(EditRange)) {
            SLog.log(std::string(MacroBody ? "SKIP macro-body-already-rewritten "
                                            : "SKIP nested-in-rewritten ") +
                      siteLoc(SM, Loc));
            return true;
        }

        std::string Reason;
        std::string Replacement = tryBuildCastReplacement(E, MacroBody, &Reason);
        if (Replacement.empty()) {
            SLog.log("FLAG " + Reason + " " + siteLoc(SM, Loc));
            return true;
        }

        bool Failed = TheRewriter.ReplaceText(EditRange, Replacement);
        if (Failed) {
            SLog.log("FLAG rewrite-failed " + siteLoc(SM, Loc));
            return true;
        }
        RewrittenOffsets.emplace_back(SM.getFileOffset(SM.getSpellingLoc(EditRange.getBegin())),
                                       SM.getFileOffset(SM.getSpellingLoc(EditRange.getEnd())));
        SLog.log(std::string("REWRITE") + (MacroBody ? "-MACRO-BODY " : " ") +
                  (CK == CK_PointerToIntegral ? "GC32 " : "GCPTR ") + siteLoc(SM, Loc) +
                  " :: " + Replacement);
        return true;
    }

    // useSpelling: when rewriting a macro's #define BODY (not an expansion site), E's own
    // getSourceRange() resolves to expansion locations that vary per call site; the *spelling*
    // location is what is actually written in the macro's definition text (including a literal
    // parameter name like `p`, not whatever argument a particular call site substituted), which is
    // exactly what belongs in the rewritten #define.
    //
    // Falls back to the *expansion* range (the call site's own verbatim text in the ultimate file,
    // e.g. `WEP_ARC_PTR(0x7)` or `DVD_BUFF2`) whenever the requested (non-spelling) mode's range is
    // a macro ID, rather than giving up: a non-macro-body call site whose *argument* is itself
    // another macro call -- object-like (`(u32) DVD_BUFF2`, `#define DVD_BUFF2 ((void*)
    // 0x80360000)`, src/game/dvd.cpp) or function-like with parameters (`(u32)
    // WEP_ARC_PTR(0x7)` -> `PL_ARC_PTR(arc, no)`'s body, src/game/objRocket.cpp) -- has a
    // subexpression whose non-argument *tokens* are spelled inside that macro's own #define text.
    // getSpellingLoc-per-endpoint used to be used here unconditionally: for a *function-like*
    // macro it collapses the whole range to the literal parameter names (`arc`, `no`) as written in
    // the header, not the actual argument text a given call site substituted (`pG->pWep`, `0x7`) --
    // measured as literal `arc`/`no` in the rewritten output, "use of undeclared identifier"
    // downstream. getExpansionRange(...).getBegin()/.getEnd() instead reports the range of tokens
    // covered by the expansion *in the ultimate (call-site) file* -- exactly the macro invocation as
    // written at the call site, valid C++ on its own and correctly substituted when the real
    // compiler later expands it, so composing it verbatim into the GC32()/GCPTR<T>() wrapper is
    // both simpler and correct; no need to reconstruct the macro's internals here (any cast genuinely
    // inside the macro's own header definition is a separate, already-skipped site -- handleCast's
    // "SKIP macro-in-header", not this function's job).
    std::string getSourceText(Expr *E, bool useSpelling = false)
    {
        SourceManager &SM = Context.getSourceManager();
        const LangOptions &LO = Context.getLangOpts();
        SourceRange R = E->getSourceRange();
        if (!R.getBegin().isMacroID() && !R.getEnd().isMacroID()) {
            CharSourceRange CR = CharSourceRange::getTokenRange(R);
            return Lexer::getSourceText(CR, SM, LO).str();
        }
        if (useSpelling) {
            SourceRange SpellR(SM.getSpellingLoc(R.getBegin()), SM.getSpellingLoc(R.getEnd()));
            CharSourceRange CR = CharSourceRange::getTokenRange(SpellR);
            return Lexer::getSourceText(CR, SM, LO).str();
        }
        SourceLocation EB = SM.getExpansionRange(R.getBegin()).getBegin();
        SourceLocation EE = SM.getExpansionRange(R.getEnd()).getEnd();
        CharSourceRange CR = CharSourceRange::getTokenRange(SourceRange(EB, EE));
        return Lexer::getSourceText(CR, SM, LO).str();
    }
};

class CastASTConsumer : public ASTConsumer {
public:
    CastASTConsumer(Rewriter &R) : TheRewriter(R) {}
    void HandleTranslationUnit(ASTContext &Context) override
    {
        CastVisitor V(TheRewriter, Context);
        V.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    Rewriter &TheRewriter;
};

class CastRewriteAction : public ASTFrontendAction {
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef File) override
    {
        TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        MainFile = std::string(File);
        return std::make_unique<CastASTConsumer>(TheRewriter);
    }

    void EndSourceFileAction() override
    {
        SourceManager &SM = TheRewriter.getSourceMgr();
        FileID MainID = SM.getMainFileID();

        std::error_code EC;
        std::string Out = OutputPath.empty() ? (MainFile + ".rewritten.cpp") : OutputPath.getValue();
        llvm::raw_fd_ostream OS(Out, EC, llvm::sys::fs::OF_None);
        if (EC) {
            llvm::errs() << "re4_cast_rewriter: cannot open output " << Out << ": " << EC.message() << "\n";
            return;
        }
        // #line back to the real file: our edits are all same-line text substitutions (no inserted
        // newlines), so a single leading #line at the top of the main-file buffer keeps every
        // subsequent line number correct without per-edit #line directives.
        OS << "#line 1 \"" << MainFile << "\"\n";
        TheRewriter.getEditBuffer(MainID).write(OS);
    }

private:
    Rewriter TheRewriter;
    std::string MainFile;
};

} // namespace

int main(int argc, const char **argv)
{
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    SLog.open(ReportPath);
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    if (!ResourceDir.empty()) {
        Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
            ("-resource-dir=" + ResourceDir.getValue()).c_str(), ArgumentInsertPosition::END));
    }
    return Tool.run(newFrontendActionFactory<CastRewriteAction>().get());
}
