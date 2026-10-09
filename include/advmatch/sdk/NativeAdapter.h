#ifndef ADVMATCH_SDK_NATIVE_ADAPTER_H
#define ADVMATCH_SDK_NATIVE_ADAPTER_H

#include "ExchangeApi.h"

namespace advmatch::api
{
    class ExchangeApi;
}

namespace advmatch::sdk
{
    // Adapt an already owned native API without creating another trading
    // session. The SDK facade closes this shared session when it is destroyed.
    ADVMATCH_SDK_API std::shared_ptr<ExchangeApi> wrapExchange(std::shared_ptr<advmatch::api::ExchangeApi> exchange);
}

#endif // ADVMATCH_SDK_NATIVE_ADAPTER_H
