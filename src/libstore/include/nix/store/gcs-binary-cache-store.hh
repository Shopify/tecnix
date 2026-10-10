#pragma once
///@file

#include "nix/store/config.hh"
#include "nix/store/http-binary-cache-store.hh"

namespace nix {

struct GcsBinaryCacheStoreConfig : HttpBinaryCacheStoreConfig
{
    GcsBinaryCacheStoreConfig(const Params & params)
        : StoreConfig(params, FilePathType::Unix)
        , HttpBinaryCacheStoreConfig(params)
    {
    }

    GcsBinaryCacheStoreConfig(ParsedURL cacheUri, const Params & params);

    GcsBinaryCacheStoreConfig(std::string_view bucketName, const Params & params);

    const Setting<std::string> endpoint{
        this,
        "",
        "endpoint",
        R"(
          The URL the GCS APIs are served at, for example a local emulator
          (`http://127.0.0.1:4443`). By default Nix uses
          `https://storage.googleapis.com`. Nix sends Google credentials to
          this endpoint when it finds them, and doesn't require them here.
        )"};

    static const std::string name()
    {
        return "GCS Binary Cache Store";
    }

    static StringSet uriSchemes();

    static std::string doc();

    std::string getHumanReadableURI() const override;

    ref<Store> openStore() const override;
};

} // namespace nix
