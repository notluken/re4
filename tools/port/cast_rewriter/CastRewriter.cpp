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
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <string>

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

    bool handleCast(ExplicitCastExpr *E)
    {
        CastKind CK = E->getCastKind();
        if (CK != CK_PointerToIntegral && CK != CK_IntegralToPointer)
            return true;

        const SourceManager &SM = Context.getSourceManager();
        SourceLocation Loc = E->getExprLoc();

        // Macro-expanded casts: left alone entirely (header-macro fix is separate, section 6).
        if (Loc.isMacroID()) {
            SLog.log("SKIP macro " + siteLoc(SM, SM.getSpellingLoc(Loc)));
            return true;
        }
        if (!SM.isWrittenInMainFile(Loc))
            return true; // only rewrite casts physically in this TU's own file, not #included ones

        if (isNestedInRewritten(E->getSourceRange())) {
            SLog.log("SKIP nested-in-rewritten " + siteLoc(SM, Loc));
            return true;
        }

        Expr *Sub = E->getSubExpr();
        std::string SubText = getSourceText(Sub);
        if (SubText.empty()) {
            SLog.log("SKIP unprintable-subexpr " + siteLoc(SM, Loc));
            return true;
        }

        std::string Replacement;
        if (CK == CK_PointerToIntegral) {
            // Decide "GC-address site" vs "genuinely wide/64-bit destination" from the *written*
            // type spelling, not Context.getTypeSize(): this tool is meant to run against the
            // default (RE4_U32_32 OFF) compile command, where the cast still type-checks (u32 is
            // the host's 8-byte `unsigned long` there, so DestWidth would read 64 for nearly every
            // site and make this check useless) -- see docs/port-phase2.md section 8. The
            // vendor's own u32/s32 typedefs are always the GameCube's 4-byte types by design
            // (include/types.h), so a spelling-based allowlist reflects the *original* semantics
            // this rewrite is targeting, independent of the host typedef's actual width.
            std::string DestSpelling = E->getTypeAsWritten().getAsString();
            static const char *WideSpellings[] = {"u64", "s64", "uintptr_t", "size_t",
                                                    "long long", "unsigned long long"};
            bool IsWide = false;
            for (const char *W : WideSpellings) {
                if (DestSpelling.find(W) != std::string::npos) {
                    IsWide = true;
                    break;
                }
            }
            if (IsWide) {
                // Ambiguous per docs/port-phase2.md section 8 ("hardest parts"): a genuinely
                // 64-bit-wide destination is more likely identity/hash use than a GC-address
                // field. Flag, don't guess.
                SLog.log("FLAG wide-dest(" + DestSpelling + ") " + siteLoc(SM, Loc));
                return true;
            }
            Replacement = "re4_port::GC32(" + SubText + ")";
            // Preserve the original cast's exact destination type spelling (s32 vs u32 vs int, ...)
            // so callers that pass the result on to a field/parameter of that exact type keep
            // compiling unchanged; GC32 always returns std::uint32_t.
            if (!DestSpelling.empty())
                Replacement = "(" + DestSpelling + ")" + Replacement;
        } else { // CK_IntegralToPointer
            QualType DestTy = E->getType();
            if (!DestTy->isPointerType()) {
                SLog.log("FLAG non-pointer-dest " + siteLoc(SM, Loc));
                return true;
            }
            QualType Pointee = DestTy->getPointeeType();
            std::string PointeeStr = Pointee.getAsString();
            // Array/function pointee types (`f32(*)[3]`, function pointers, ...) can't be spelled
            // as `GCPTR<T>`'s simple `T*` return (Pointee.getAsString() + "*" is not valid syntax
            // for them, e.g. "f32[3]*" -- measured, caught in the 20-site spot check: this produced
            // a real compile error). Flag instead of guessing at the declarator-around-identifier
            // spelling those need.
            if (PointeeStr.empty() || PointeeStr.find("unnamed") != std::string::npos ||
                Pointee->isArrayType() || Pointee->isFunctionType()) {
                SLog.log("FLAG unprintable-pointee " + siteLoc(SM, Loc));
                return true;
            }
            Replacement = "re4_port::GCPTR<" + PointeeStr + ">((std::uint32_t)(" + SubText + "))";
        }

        SourceRange FullRange = E->getSourceRange();
        bool Failed = TheRewriter.ReplaceText(FullRange, Replacement);
        if (Failed) {
            SLog.log("FLAG rewrite-failed " + siteLoc(SM, Loc));
            return true;
        }
        RewrittenOffsets.emplace_back(SM.getFileOffset(SM.getSpellingLoc(FullRange.getBegin())),
                                       SM.getFileOffset(SM.getSpellingLoc(FullRange.getEnd())));
        SLog.log(std::string("REWRITE ") + (CK == CK_PointerToIntegral ? "GC32 " : "GCPTR ") +
                  siteLoc(SM, Loc) + " :: " + Replacement);
        return true;
    }

    std::string getSourceText(Expr *E)
    {
        SourceManager &SM = Context.getSourceManager();
        const LangOptions &LO = Context.getLangOpts();
        SourceRange R = E->getSourceRange();
        if (R.getBegin().isMacroID() || R.getEnd().isMacroID())
            return {}; // sub-expression itself macro-spelled: too risky to splice, flag via caller
        CharSourceRange CR = CharSourceRange::getTokenRange(R);
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
