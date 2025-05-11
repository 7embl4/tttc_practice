#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace {

class TypeAnalyzerVisitor : public RecursiveASTVisitor<TypeAnalyzerVisitor> {
public:
    explicit TypeAnalyzerVisitor(ASTContext *Context) : Context(Context) {}

    bool VisitCXXRecordDecl(CXXRecordDecl *Declaration) {
        llvm::outs() << Declaration->getQualifiedNameAsString();
        if (Declaration->getNumBases() != 0) {
            for (auto &Base : Declaration->bases()) {
                llvm::outs() << " -> " << Base.getType().getAsString();
            }
        }
        llvm::outs() << "\n";

        PrintFields(Declaration);
        llvm::outs() << "|\n";
        PrintMethods(Declaration);

        return true;
    }

private:
    ASTContext *Context;

    void PrintFields(CXXRecordDecl *Declaration) {
        if (Declaration->field_empty()) {
            return;
        }

        llvm::outs() << "|_Fields\n";
        for (auto *Field : Declaration->fields()) {
            llvm::outs() << "| |_ " << Field->getNameAsString()
                         << " (" << Field->getType().getAsString();
            
            auto access = Field->getAccess();
            switch (access)
            {
            case clang::AS_public:
                llvm::outs() << "|public";
                break;
            case clang::AS_private:
                llvm::outs() << "|private";
                break;
            case clang::AS_protected:
                llvm::outs() << "|protected";
                break;
            }
            llvm::outs() << ")\n";
        }
    }

    void PrintMethods(CXXRecordDecl *Declaration) {
        if (Declaration->method_begin() == Declaration->method_end()) {
            return;
        }

        llvm::outs() << "|_Methods\n";
        for (auto *Method : Declaration->methods()) {
            if (Method->isUserProvided()) {
                llvm::outs() << "| |_ " << Method->getQualifiedNameAsString()
                             << " (" << Method->getReturnType().getAsString();
                
                auto access = Method->getAccess();
                switch (access)
                {
                case clang::AS_public:
                    llvm::outs() << "|public";
                    break;
                case clang::AS_private:
                    llvm::outs() << "|private";
                    break;
                case clang::AS_protected:
                    llvm::outs() << "|protected";
                    break;
                }
                
                if (Method->isVirtual()) {
                    llvm::outs() << "|virtual";
                }
                if (Method->isPureVirtual()) {
                    llvm::outs() << "|pure";
                }
                if (Method->size_overridden_methods() != 0) {
                    llvm::outs() << "|override";
                }
                llvm::outs() << ")\n";
            }
        }
    }
};

class TypeAnalyzerConsumer : public ASTConsumer {
public:
    explicit TypeAnalyzerConsumer(ASTContext *Context) : Visitor(Context) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    TypeAnalyzerVisitor Visitor;
};

class TypeAnalyzerPlugin : public PluginASTAction {
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, llvm::StringRef) override {
        return std::make_unique<TypeAnalyzerConsumer>(&CI.getASTContext());
    }

    bool ParseArgs(const CompilerInstance &CI, const std::vector<std::string> &args) override {
        return true;
    }
};

}

static FrontendPluginRegistry::Add<TypeAnalyzerPlugin>
    X("type-analyzer", "Analyze user-defined types (fields, methods, base classes)");
