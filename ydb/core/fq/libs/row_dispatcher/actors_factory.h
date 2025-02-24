#pragma once

#include "common.h"

#include <ydb/core/fq/libs/config/protos/row_dispatcher.pb.h>
#include <util/generic/ptr.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/public/sdk/cpp/client/ydb_driver/driver.h>
#include <ydb/library/yql/providers/pq/provider/yql_pq_gateway.h>

namespace NFq::NRowDispatcher {

struct IActorFactory : public TThrRefBase {
    using TPtr = TIntrusivePtr<IActorFactory>;

    virtual NActors::TActorId RegisterTopicSession(
        const TString& topicPath,
        const TString& endpoint,
        const TString& database,
        const NConfig::TRowDispatcherConfig& config,
        NActors::TActorId rowDispatcherActorId,
        ui32 partitionId,
        NYdb::TDriver driver,
        std::shared_ptr<NYdb::ICredentialsProviderFactory> credentialsProviderFactory,
        IPureCalcProgramFactory::TPtr pureCalcProgramFactory,
        const ::NMonitoring::TDynamicCounterPtr& counters,
        const NYql::IPqGateway::TPtr& pqGateway,
        ui64 maxBufferSize) const = 0;
};

IActorFactory::TPtr CreateActorFactory();

}
