#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "common/typecase.h"
#include "core/lsp/QueryResponse.h"
#include "main/lsp/lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {

namespace {

vector<core::SymbolRef> ancestorsImpl(const core::GlobalState &gs, core::SymbolRef sym, vector<core::SymbolRef> &&acc) {
    // The implementation here is similar to Symbols::derivesFrom.
    ENFORCE(sym.data(gs)->isClassOrModuleLinearizationComputed());
    acc.emplace_back(sym);

    for (auto mixin : sym.data(gs)->mixins()) {
        acc.emplace_back(mixin);
    }

    if (sym.data(gs)->superClass().exists()) {
        return ancestorsImpl(gs, sym.data(gs)->superClass(), move(acc));
    } else {
        return move(acc);
    }
}

// Basically the same as Module#ancestors from Ruby--but don't depend on it being exactly equal.
// For us, it's just something that's vaguely ordered from "most specific" to "least specific" ancestor.
vector<core::SymbolRef> ancestors(const core::GlobalState &gs, core::SymbolRef receiver) {
    return ancestorsImpl(gs, receiver, vector<core::SymbolRef>{});
}

struct SimilarMethod final {
    int depth;
    core::SymbolRef receiver;
    core::SymbolRef method;

    // Populated later
    core::TypePtr receiverType = nullptr;
    shared_ptr<core::TypeConstraint> constr = nullptr;
};

using SimilarMethodsByName = UnorderedMap<core::NameRef, vector<SimilarMethod>>;

// First of pair is "found at this depth in the ancestor hierarchy"
// Second of pair is method symbol found at that depth, with name similar to prefix.
SimilarMethodsByName similarMethodsForClass(const core::GlobalState &gs, core::SymbolRef receiver, string_view prefix) {
    auto result = SimilarMethodsByName{};

    int depth = -1;
    for (auto ancestor : ancestors(gs, receiver)) {
        depth++;
        for (auto [memberName, memberSymbol] : ancestor.data(gs)->members()) {
            if (!memberSymbol.data(gs)->isMethod()) {
                continue;
            }

            if (hasSimilarName(gs, memberName, prefix)) {
                // Creates the the list if it does not exist
                result[memberName].emplace_back(SimilarMethod{depth, receiver, memberSymbol});
            }
        }
    }

    return result;
}

// Unconditionally creates an intersection of the methods
// (for both union and intersection types, it's only valid to call a method by name if it exists on all components)
SimilarMethodsByName mergeSimilarMethods(SimilarMethodsByName left, SimilarMethodsByName right) {
    auto result = SimilarMethodsByName{};

    for (auto [methodName, leftSimilarMethods] : left) {
        if (right.find(methodName) != right.end()) {
            for (auto similarMethod : leftSimilarMethods) {
                result[methodName].emplace_back(similarMethod);
            }
            for (auto similarMethod : right[methodName]) {
                result[methodName].emplace_back(similarMethod);
            }
        }
    }
    return result;
}

SimilarMethodsByName similarMethodsForReceiver(const core::GlobalState &gs, const core::TypePtr receiver,
                                               string_view prefix) {
    auto result = SimilarMethodsByName{};

    typecase(
        receiver.get(), [&](core::ClassType *type) { result = similarMethodsForClass(gs, type->symbol, prefix); },
        [&](core::AppliedType *type) { result = similarMethodsForClass(gs, type->klass, prefix); },
        [&](core::AndType *type) {
            result = mergeSimilarMethods(similarMethodsForReceiver(gs, type->left, prefix),
                                         similarMethodsForReceiver(gs, type->right, prefix));
        },
        [&](core::ProxyType *type) { result = similarMethodsForReceiver(gs, type->underlying(), prefix); },
        [&](core::Type *type) { return; });

    return result;
}

// Walk a core::DispatchResult to find methods similar to `prefix` on any of its DispatchComponents' receivers.
SimilarMethodsByName allSimilarMethods(const core::GlobalState &gs, core::DispatchResult &dispatchResult,
                                       string_view prefix) {
    auto result = similarMethodsForReceiver(gs, dispatchResult.main.receiver, prefix);

    // Convert to shared_ptr and take ownership
    shared_ptr<core::TypeConstraint> constr = move(dispatchResult.main.constr);

    for (auto &[methodName, similarMethods] : result) {
        for (auto &similarMethod : similarMethods) {
            ENFORCE(similarMethod.receiverType == nullptr, "About to overwrite non-null receiverType");
            similarMethod.receiverType = dispatchResult.main.receiver;

            ENFORCE(similarMethod.constr == nullptr, "About to overwrite non-null constr");
            similarMethod.constr = constr;
        }
    }

    if (dispatchResult.secondary != nullptr) {
        // Right now we completely ignore the secondaryKind (either AND or OR), and always intersect.
        // (See comment above mergeSimilarMethods)
        result = mergeSimilarMethods(result, allSimilarMethods(gs, *dispatchResult.secondary, prefix));
    }

    return result;
}

string methodSnippet(const core::GlobalState &gs, core::SymbolRef method) {
    auto shortName = method.data(gs)->name.data(gs)->shortName(gs);
    vector<string> typeAndArgNames;

    int i = 1;
    if (method.data(gs)->isMethod()) {
        for (auto &argSym : method.data(gs)->arguments()) {
            string s;
            if (argSym.flags.isBlock) {
                continue;
            }
            if (argSym.flags.isKeyword) {
                absl::StrAppend(&s, argSym.name.data(gs)->shortName(gs), ": ");
            }
            if (argSym.type) {
                absl::StrAppend(&s, "${", i++, ":", argSym.type->show(gs), "}");
            } else {
                absl::StrAppend(&s, "${", i++, "}");
            }
            typeAndArgNames.emplace_back(s);
        }
    }

    return fmt::format("{}({}){}", shortName, fmt::join(typeAndArgNames, ", "), "${0}");
}

} // namespace

unique_ptr<CompletionItem> LSPLoop::getCompletionItem(const core::GlobalState &gs, core::SymbolRef what,
                                                      core::TypePtr receiverType,
                                                      const core::TypeConstraint *constraint, size_t sortIdx) const {
    ENFORCE(what.exists());
    auto item = make_unique<CompletionItem>(string(what.data(gs)->name.data(gs)->shortName(gs)));

    // Completion items are sorted by sortText if present, or label if not. We unconditionally use an index to sort.
    // If we ever have 100,000+ items in the completion list, we'll need to bump the padding here.
    item->sortText = fmt::format("{:06d}", sortIdx);

    auto resultType = what.data(gs)->resultType;
    if (!resultType) {
        resultType = core::Types::untypedUntracked();
    }
    if (what.data(gs)->isMethod()) {
        item->kind = CompletionItemKind::Method;
        if (what.exists()) {
            item->detail = methodDetail(gs, what, receiverType, nullptr, constraint);
        }
        if (config.clientCompletionItemSnippetSupport) {
            item->insertTextFormat = InsertTextFormat::Snippet;
            item->insertText = methodSnippet(gs, what);
        } else {
            item->insertTextFormat = InsertTextFormat::PlainText;
            item->insertText = string(what.data(gs)->name.data(gs)->shortName(gs));
        }

        optional<string> documentation = nullopt;
        if (what.data(gs)->loc().file().exists()) {
            documentation =
                findDocumentation(what.data(gs)->loc().file().data(gs).source(), what.data(gs)->loc().beginPos());
        }
        if (documentation != nullopt) {
            if (documentation->find("@deprecated") != documentation->npos) {
                item->deprecated = true;
            }
            item->documentation =
                make_unique<MarkupContent>(config.clientCompletionItemMarkupKind, documentation.value());
        }
    } else if (what.data(gs)->isStaticField()) {
        item->kind = CompletionItemKind::Constant;
        item->detail = resultType->show(gs);
    } else if (what.data(gs)->isClassOrModule()) {
        item->kind = CompletionItemKind::Class;
    }
    return item;
}

void LSPLoop::findSimilarConstantOrIdent(const core::GlobalState &gs, const core::TypePtr receiverType,
                                         vector<unique_ptr<CompletionItem>> &items) const {
    if (auto c = core::cast_type<core::ClassType>(receiverType.get())) {
        auto pattern = c->symbol.data(gs)->name.data(gs)->shortName(gs);
        logger->debug("Looking for constant similar to {}", pattern);
        core::SymbolRef owner = c->symbol;
        do {
            owner = owner.data(gs)->owner;
            for (auto member : owner.data(gs)->membersStableOrderSlow(gs)) {
                auto sym = member.second;
                if (sym.exists() && (sym.data(gs)->isClassOrModule() || sym.data(gs)->isStaticField()) &&
                    sym.data(gs)->name.data(gs)->kind == core::NameKind::CONSTANT &&
                    // hide singletons
                    hasSimilarName(gs, sym.data(gs)->name, pattern)) {
                    items.push_back(getCompletionItem(gs, sym, receiverType, nullptr, items.size()));
                }
            }
        } while (owner != core::Symbols::root());
    }
}

LSPResult LSPLoop::handleTextDocumentCompletion(unique_ptr<core::GlobalState> gs, const MessageId &id,
                                                const CompletionParams &params) const {
    auto response = make_unique<ResponseMessage>("2.0", id, LSPMethod::TextDocumentCompletion);
    if (!config.opts.lspAutocompleteEnabled) {
        response->error =
            make_unique<ResponseError>((int)LSPErrorCodes::InvalidRequest,
                                       "The `Autocomplete` LSP feature is experimental and disabled by default.");
        return LSPResult::make(move(gs), move(response));
    }

    prodCategoryCounterInc("lsp.messages.processed", "textDocument.completion");

    auto result =
        setupLSPQueryByLoc(move(gs), params.textDocument->uri, *params.position, LSPMethod::TextDocumentCompletion);
    gs = move(result.gs);

    if (result.error) {
        // An error happened while setting up the query.
        response->error = move(result.error);
        return LSPResult::make(move(gs), move(response));
    }

    auto &queryResponses = result.responses;
    vector<unique_ptr<CompletionItem>> items;
    if (!queryResponses.empty()) {
        auto resp = move(queryResponses[0]);

        if (auto sendResp = resp->isSend()) {
            auto prefix = sendResp->callerSideName.data(*gs)->shortName(*gs);
            logger->debug("Looking for method similar to {}", prefix);

            auto similarMethodsByName = allSimilarMethods(*gs, *sendResp->dispatchResult, prefix);
            for (auto &[methodName, similarMethods] : similarMethodsByName) {
                fast_sort(similarMethods, [&](const auto &left, const auto &right) -> bool {
                    if (left.depth != right.depth) {
                        return left.depth < right.depth;
                    }

                    return left.method._id < right.method._id;
                });
            }

            auto deduped = vector<SimilarMethod>{};
            for (auto &[methodName, similarMethods] : similarMethodsByName) {
                if (methodName.data(*gs)->kind == core::NameKind::UNIQUE &&
                    methodName.data(*gs)->unique.uniqueNameKind == core::UniqueNameKind::MangleRename) {
                    // It's possible we want to ignore more things here. But note that we *don't* want to ignore all
                    // unique names, because we want each overload to show up but those use unique names.
                    continue;
                }

                // Since each list is sorted by depth, taking the first elem dedups by depth within each name.
                deduped.emplace_back(similarMethods[0]);
            }

            fast_sort(deduped, [&](const auto &left, const auto &right) -> bool {
                if (left.depth != right.depth) {
                    return left.depth < right.depth;
                }

                auto leftShortName = left.method.data(*gs)->name.data(*gs)->shortName(*gs);
                auto rightShortName = right.method.data(*gs)->name.data(*gs)->shortName(*gs);
                if (leftShortName != rightShortName) {
                    if (absl::StartsWith(leftShortName, prefix) && !absl::StartsWith(rightShortName, prefix)) {
                        return true;
                    }
                    if (!absl::StartsWith(leftShortName, prefix) && absl::StartsWith(rightShortName, prefix)) {
                        return false;
                    }

                    return leftShortName < rightShortName;
                }

                return left.method._id < right.method._id;
            });

            for (auto &similarMethod : deduped) {
                items.push_back(getCompletionItem(*gs, similarMethod.method, similarMethod.receiverType,
                                                  similarMethod.constr.get(), items.size()));
            }
        } else if (auto identResp = resp->isIdent()) {
            findSimilarConstantOrIdent(*gs, identResp->retType.type, items);
        } else if (auto constantResp = resp->isConstant()) {
            findSimilarConstantOrIdent(*gs, constantResp->retType.type, items);
        }
    }

    response->result = make_unique<CompletionList>(false, move(items));
    return LSPResult::make(move(gs), move(response));
}

} // namespace sorbet::realmain::lsp
