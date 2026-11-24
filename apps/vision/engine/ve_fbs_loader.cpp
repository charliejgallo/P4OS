/*
 * P4OS - Vision, the engine: esp-dl's FbsLoader, reduced to nothing.
 *
 * esp-dl's own loader reads models out of the flash (rodata or a partition)
 * or a card, and decrypts them with mbedtls; none of that is reachable from
 * a module, and the engine does not need it: it reads the pack itself and
 * hands dl::Model an FbsModel over the buffer (ve_engine.cpp). The class
 * stays because dl::Model's constructors that take a path name it; those
 * are never called here, and if one were it would fail cleanly.
 */
#include "fbs_loader.hpp"

namespace fbs {

FbsLoader::FbsLoader(const char *name, model_location_type_t location) :
    m_mmap_handle(nullptr), m_location(location), m_fbs_buf(name), m_format(FBS_FILE_FORMAT_UNK)
{
}

FbsLoader::~FbsLoader() {}

FbsModel *FbsLoader::load(const int model_index, const uint8_t *key, bool param_copy)
{
    (void)model_index;
    (void)key;
    (void)param_copy;
    ESP_LOGE("FbsLoader", "models are loaded by the Vision engine, not from a path");
    return nullptr;
}

FbsModel *FbsLoader::load(const uint8_t *key, bool param_copy)
{
    return load(0, key, param_copy);
}

FbsModel *FbsLoader::load(const char *model_name, const uint8_t *key, bool param_copy)
{
    (void)model_name;
    return load(0, key, param_copy);
}

const char *FbsLoader::get_model_location_string()
{
    return "the Vision engine's pack";
}

} // namespace fbs
