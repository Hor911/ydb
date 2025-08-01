#pragma once

#include <yql/essentials/core/expr_nodes/yql_expr_nodes.h>
#include <yql/essentials/providers/common/mkql/yql_provider_mkql.h>

#include <yql/essentials/ast/yql_expr.h>
#include <yql/essentials/minikql/mkql_node_visitor.h>
#include <yql/essentials/minikql/computation/mkql_computation_node.h>
#include <yql/essentials/minikql/mkql_function_registry.h>
#include <yql/essentials/minikql/mkql_stats_registry.h>
#include <yql/essentials/minikql/mkql_node.h>
#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/public/udf/udf_validate.h>
#include <yql/essentials/public/langver/yql_langver.h>

#include <library/cpp/random_provider/random_provider.h>
#include <library/cpp/time_provider/time_provider.h>

#include <util/generic/ptr.h>
#include <util/generic/vector.h>

#include <tuple>

namespace NYql {

class TPatternCache;

class TLambdaBuilder {
public:
    using TArgumentsMap = NCommon::TMkqlBuildContext::TArgumentsMap;

    TLambdaBuilder(const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
        NKikimr::NMiniKQL::TScopedAlloc& alloc,
        const NKikimr::NMiniKQL::TTypeEnvironment* env = nullptr,
        const TIntrusivePtr<IRandomProvider>& randomProvider = {},
        const TIntrusivePtr<ITimeProvider>& timeProvider = {},
        NKikimr::NMiniKQL::IStatsRegistry* jobStats = nullptr,
        NKikimr::NUdf::ICountersProvider* counters = nullptr,
        const NKikimr::NUdf::ISecureParamsProvider *secureParamsProvider = nullptr,
        const NKikimr::NUdf::ILogProvider* logProvider = nullptr,
        TLangVersion langver = UnknownLangVersion);

    ~TLambdaBuilder();

    const NKikimr::NMiniKQL::TTypeEnvironment& GetTypeEnvironment() const {
        if (!Env) {
            Env = CreateTypeEnv();
        }
        return *Env;
    }

    const NKikimr::NMiniKQL::IFunctionRegistry& GetFunctionRegistry() const {
        return *FunctionRegistry;
    }

    NKikimr::NMiniKQL::TRuntimeNode BuildLambda(
        const NCommon::IMkqlCallableCompiler& compiler,
        const TExprNode::TPtr& lambdaNode,
        TExprContext& exprCtx,
        TArgumentsMap&& arguments = {}
    ) const;

    NKikimr::NMiniKQL::TRuntimeNode TransformAndOptimizeProgram(NKikimr::NMiniKQL::TRuntimeNode root,
        NKikimr::NMiniKQL::TCallableVisitFuncProvider funcProvider);

    THolder<NKikimr::NMiniKQL::IComputationGraph> BuildGraph(
        const NKikimr::NMiniKQL::TComputationNodeFactory& factory,
        NKikimr::NUdf::EValidateMode validateMode,
        NKikimr::NUdf::EValidatePolicy validatePolicy,
        const TString& optLLVM,
        NKikimr::NMiniKQL::EGraphPerProcess graphPerProcess,
        NKikimr::NMiniKQL::TExploringNodeVisitor& explorer,
        NKikimr::NMiniKQL::TRuntimeNode root) const;
    THolder<NKikimr::NMiniKQL::IComputationGraph> BuildGraph(
        const NKikimr::NMiniKQL::TComputationNodeFactory& factory,
        NKikimr::NUdf::EValidateMode validateMode,
        NKikimr::NUdf::EValidatePolicy validatePolicy,
        const TString& optLLVM,
        NKikimr::NMiniKQL::EGraphPerProcess graphPerProcess,
        NKikimr::NMiniKQL::TExploringNodeVisitor& explorer,
        NKikimr::NMiniKQL::TRuntimeNode& root,
        std::vector<NKikimr::NMiniKQL::TNode*>&& entryPoints,
        TIntrusivePtr<IRandomProvider> randomProvider = {},
        TIntrusivePtr<ITimeProvider> timeProvider = {}) const;
    std::tuple<
        THolder<NKikimr::NMiniKQL::IComputationGraph>,
        TIntrusivePtr<IRandomProvider>,
        TIntrusivePtr<ITimeProvider>
    >
        BuildLocalGraph(
        const NKikimr::NMiniKQL::TComputationNodeFactory& factory,
        NKikimr::NUdf::EValidateMode validateMode,
        NKikimr::NUdf::EValidatePolicy validatePolicy,
        const TString& optLLVM,
        NKikimr::NMiniKQL::EGraphPerProcess graphPerProcess,
        NKikimr::NMiniKQL::TExploringNodeVisitor& explorer,
        NKikimr::NMiniKQL::TRuntimeNode root) const;

    NKikimr::NMiniKQL::TRuntimeNode MakeTuple(const TVector<NKikimr::NMiniKQL::TRuntimeNode>& items) const;

    NKikimr::NMiniKQL::TRuntimeNode UpdateLambdaCode(TString& code, size_t& nodes, NKikimr::NMiniKQL::TCallableVisitFuncProvider funcProvider);
    NKikimr::NMiniKQL::TRuntimeNode Deserialize(const TString& code);
    std::pair<TString, size_t> Serialize(NKikimr::NMiniKQL::TRuntimeNode rootNode);

protected:
    const NKikimr::NMiniKQL::IFunctionRegistry* FunctionRegistry;
    NKikimr::NMiniKQL::TScopedAlloc& Alloc;
    const TIntrusivePtr<IRandomProvider> RandomProvider;
    const TIntrusivePtr<ITimeProvider> TimeProvider;
    NKikimr::NMiniKQL::IStatsRegistry* const JobStats;
    NKikimr::NUdf::ICountersProvider* const Counters;
    const NKikimr::NUdf::ISecureParamsProvider* SecureParamsProvider;
    const NKikimr::NUdf::ILogProvider* LogProvider;
    const TLangVersion LangVer;

    /// TODO: remove?
    void SetExternalEnv(const NKikimr::NMiniKQL::TTypeEnvironment* env);
private:
    const NKikimr::NMiniKQL::TTypeEnvironment* CreateTypeEnv() const;
    mutable std::shared_ptr<NKikimr::NMiniKQL::TTypeEnvironment> InjectedEnvPtr;
    mutable std::shared_ptr<NKikimr::NMiniKQL::TTypeEnvironment> EnvPtr;
    mutable const NKikimr::NMiniKQL::TTypeEnvironment* Env;
};

class TGatewayLambdaBuilder : public TLambdaBuilder {
public:
    TGatewayLambdaBuilder(const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
        NKikimr::NMiniKQL::TScopedAlloc& alloc,
        const NKikimr::NMiniKQL::TTypeEnvironment* env = nullptr,
        const TIntrusivePtr<IRandomProvider>& randomProvider = {},
        const TIntrusivePtr<ITimeProvider>& timeProvider = {},
        NKikimr::NMiniKQL::IStatsRegistry* jobStats = nullptr,
        NKikimr::NUdf::ICountersProvider* counters = nullptr,
        const NKikimr::NUdf::ISecureParamsProvider* secureParamsProvider = nullptr,
        const NKikimr::NUdf::ILogProvider* logProvider = nullptr,
        TLangVersion langver = UnknownLangVersion);

    TString BuildLambdaWithIO(const TString& prefix, const NCommon::IMkqlCallableCompiler& compiler, NNodes::TCoLambda lambda, TExprContext& exprCtx);
};

} // namespace NYql
