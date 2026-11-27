/*
 * P4OS - Vision, the engine: esp-dl's model reader (fbs::FbsModel), written
 * here because esp-dl ships it only as a library built for flash (not
 * position-independent, so it cannot go into a module) and only for the
 * chips (so it cannot run in the simulator).
 *
 * The models are FlatBuffers of the schema espdl/fbs_loader/espdl.fbs, read
 * in place with the small reader below: a table is a signed offset to its
 * vtable, the vtable a list of 16-bit field offsets; strings and vectors are
 * reached through 32-bit offsets and start with their length. Nothing is
 * copied except what esp-dl asks for as a tensor, and then only when
 * m_param_copy says so (the weights stay in the PSRAM buffer of the pack).
 *
 * What each method answers follows the class's own header
 * (fbs_loader/include/fbs_model.hpp) and esp-dl's callers; every model in
 * the packs is checked against the test vectors it carries
 * (tools/ve_check.cpp, dl::Model::test()).
 */
#include "fbs_model.hpp"

#include <algorithm>
#include <set>
#include <string.h>

static const char *TAG = "FbsModel";

namespace {

/* ---- the FlatBuffers reader -------------------------------------------- */

inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

inline uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

struct Vec;

/* A table: its fields by id (the order in the schema). */
struct Tab {
    const uint8_t *p = nullptr;

    explicit operator bool() const { return p != nullptr; }

    const uint8_t *field(int id) const
    {
        if (!p) return nullptr;
        const uint8_t *vt = p - (int32_t)rd32(p);
        uint16_t vsize = rd16(vt);
        uint16_t vo = (uint16_t)(4 + 2 * id);
        if (vo + 2 > vsize) return nullptr;
        uint16_t off = rd16(vt + vo);
        return off ? p + off : nullptr;
    }
    template <typename T> T scalar(int id, T def = T()) const
    {
        const uint8_t *f = field(id);
        if (!f) return def;
        T v;
        memcpy(&v, f, sizeof v);
        return v;
    }
    const uint8_t *ref(int id) const
    {
        const uint8_t *f = field(id);
        return f ? f + rd32(f) : nullptr;
    }
    Tab table(int id) const { return Tab{ref(id)}; }
    std::string str(int id) const
    {
        const uint8_t *s = ref(id);
        return s ? std::string((const char *)s + 4, rd32(s)) : std::string();
    }
    Vec vec(int id) const;
};

/* A vector: its length, and its elements as scalars, tables or strings. */
struct Vec {
    const uint8_t *p = nullptr;

    uint32_t size() const { return p ? rd32(p) : 0; }
    const uint8_t *data() const { return p ? p + 4 : nullptr; }
    template <typename T> T at(uint32_t i) const
    {
        T v;
        memcpy(&v, p + 4 + i * sizeof(T), sizeof v);
        return v;
    }
    Tab table(uint32_t i) const
    {
        const uint8_t *e = p + 4 + 4 * i;
        return Tab{e + rd32(e)};
    }
    std::string str(uint32_t i) const
    {
        const uint8_t *e = p + 4 + 4 * i;
        const uint8_t *s = e + rd32(e);
        return std::string((const char *)s + 4, rd32(s));
    }
};

Vec Tab::vec(int id) const
{
    return Vec{ref(id)};
}

/* Field ids, as declared in espdl.fbs. */
enum { MODEL_DOC = 6, MODEL_VERSION = 5, MODEL_GRAPH = 7, MODEL_META = 8 };
enum {
    GRAPH_NODE = 0,
    GRAPH_NAME = 1,
    GRAPH_INIT = 2,
    GRAPH_INPUT = 4,
    GRAPH_OUTPUT = 5,
    GRAPH_VALUE_INFO = 6,
    GRAPH_TEST_IN = 8,
    GRAPH_TEST_OUT = 9
};
enum { NODE_INPUT = 0, NODE_OUTPUT = 1, NODE_NAME = 2, NODE_OP = 3, NODE_ATTR = 5 };
enum { ATTR_NAME = 0, ATTR_F = 4, ATTR_I = 5, ATTR_S = 6, ATTR_T = 7, ATTR_FLOATS = 10, ATTR_INTS = 11 };
enum { VI_NAME = 0, VI_TYPE = 1, VI_EXPS = 3 };
enum {
    T_DIMS = 0,
    T_DTYPE = 1,
    T_FLOAT = 2,
    T_INT32 = 3,
    T_STRING = 4,
    T_INT64 = 5,
    T_NAME = 6,
    T_RAW = 8,
    T_EXTERNAL = 9,
    T_DOUBLE = 11,
    T_UINT64 = 12,
    T_EXPS = 13
};
enum { TYPE_KIND = 0, TYPE_VALUE = 1 };   /* TypeInfo: the union's type and value */
enum { KIND_TENSOR = 1 };                 /* TypeInfoValue.tensor_type           */
enum { TTS_ELEM = 0, TTS_SHAPE = 1 };     /* TensorTypeAndShape                  */
enum { DIM_VALUE = 0 };                   /* Dimension                           */
enum { DV_TYPE = 0, DV_VALUE = 1 };       /* DimensionValue                      */
enum { DV_KIND_VALUE = 1 };               /* DimensionValueType.VALUE            */
enum { KV_KEY = 0, KV_VALUE = 1 };        /* StringStringEntry                   */

Tab as_tab(const void *p)
{
    return Tab{(const uint8_t *)p};
}

dl::dtype_t to_dtype(int32_t t)
{
    /* the schema's TensorDataType and dl::dtype_t number the types alike */
    return (t >= dl::DATA_TYPE_MIN && t <= dl::DATA_TYPE_MAX) ? (dl::dtype_t)t : dl::DATA_TYPE_UNDEFINED;
}

std::vector<int> ints_of(Vec v)
{
    std::vector<int> out(v.size());
    for (uint32_t i = 0; i < v.size(); i++) out[i] = (int)v.at<int64_t>(i);
    return out;
}

std::vector<int> tensor_shape(Tab t)
{
    return ints_of(t.vec(T_DIMS));
}

std::vector<int> tensor_exponents(Tab t)
{
    return ints_of(t.vec(T_EXPS));
}

/* The tensor's data: the first of its data vectors that is there. */
const void *tensor_raw(Tab t)
{
    static const int order[] = {T_RAW, T_FLOAT, T_INT32, T_STRING, T_INT64, T_EXTERNAL, T_DOUBLE, T_UINT64};
    for (int id : order) {
        Vec v = t.vec(id);
        if (v.p) return v.data();
    }
    ESP_LOGW(TAG, "The tensor(%s) raw data is nullptr", t.str(T_NAME).c_str());
    return nullptr;
}

dl::TensorBase *make_tensor(Tab t, bool deep, uint32_t caps)
{
    std::vector<int> shape = tensor_shape(t);
    std::vector<int> exps = tensor_exponents(t);
    const void *raw = tensor_raw(t);
    dl::dtype_t dtype = to_dtype(t.scalar<int32_t>(T_DTYPE));
    if (exps.empty() || (!shape.empty() && shape[0] <= 0) || !raw || dtype == dl::DATA_TYPE_UNDEFINED) {
        ESP_LOGE(TAG,
                 "(%s) Unsupported exponent size, data type or abnormal shape for tensor %s",
                 __func__,
                 t.str(T_NAME).c_str());
        return nullptr;
    }
    /* TensorBase has its own operator new: aligned, as esp-dl frees it */
    dl::TensorBase *tb = new dl::TensorBase(shape, raw, exps, dtype, deep, caps);
    if (!tb) return nullptr;
    if (!tb->data || !tb->exponent.is_valid()) {
        ESP_LOGE(TAG, "Failed to load the %d per-channel exponents of tensor %s", (int)exps.size(),
                 t.str(T_NAME).c_str());
        delete tb;
        return nullptr;
    }
    return tb;
}

/* The TensorTypeAndShape of a ValueInfo, or an empty table. */
Tab value_tensor_type(Tab vi)
{
    Tab ti = vi.table(VI_TYPE);
    if (!ti) return Tab{};
    uint8_t kind = ti.scalar<uint8_t>(TYPE_KIND);
    if (kind != KIND_TENSOR) {
        ESP_LOGE(TAG, "The value info type(%d) don't be support now", kind);
        return Tab{};
    }
    return ti.table(TYPE_VALUE);
}

Tab find_attr(Tab node, const std::string &name)
{
    Vec attrs = node.vec(NODE_ATTR);
    for (uint32_t i = 0; i < attrs.size(); i++) {
        Tab a = attrs.table(i);
        if (a.str(ATTR_NAME) == name) return a;
    }
    return Tab{};
}

template <typename M> const void *lookup(const M &map, const std::string &name)
{
    auto it = map.find(name);
    return it == map.end() ? nullptr : it->second;
}

} // namespace

namespace fbs {

FbsModel::FbsModel(const void *data,
                   size_t size,
                   model_location_type_t location,
                   bool encrypt,
                   bool rodata_move,
                   bool auto_free,
                   bool param_copy) :
    m_param_copy(param_copy),
    m_location(location),
    m_encrypt(encrypt),
    m_rodata_move(rodata_move),
    m_auto_free(auto_free),
    m_size(size),
    m_data((const uint8_t *)data),
    m_model(nullptr)
{
    if (!data) {
        ESP_LOGW(TAG, "Model data is null");
        return;
    }
    m_model = m_data + rd32(m_data);
}

FbsModel::~FbsModel()
{
    clear_map();
    if (m_auto_free && m_data) heap_caps_free((void *)m_data);
}

void FbsModel::load_map()
{
    clear_map();
    Tab graph = as_tab(m_model).table(MODEL_GRAPH);
    if (!graph) return;
    Vec nodes = graph.vec(GRAPH_NODE);
    for (uint32_t i = 0; i < nodes.size(); i++) {
        Tab n = nodes.table(i);
        m_name_to_node_map[n.str(NODE_NAME)] = n.p;
    }
    Vec inits = graph.vec(GRAPH_INIT);
    for (uint32_t i = 0; i < inits.size(); i++) {
        Tab t = inits.table(i);
        m_name_to_initial_tensor_map[t.str(T_NAME)] = t.p;
    }
    for (int id : {GRAPH_VALUE_INFO, GRAPH_INPUT, GRAPH_OUTPUT}) {
        Vec vis = graph.vec(id);
        for (uint32_t i = 0; i < vis.size(); i++) {
            Tab vi = vis.table(i);
            m_name_to_value_info_map[vi.str(VI_NAME)] = vi.p;
        }
    }
    Vec tin = graph.vec(GRAPH_TEST_IN);
    for (uint32_t i = 0; i < tin.size(); i++) {
        Tab t = tin.table(i);
        m_name_to_test_inputs_value_map[t.str(T_NAME)] = t.p;
    }
    Vec tout = graph.vec(GRAPH_TEST_OUT);
    for (uint32_t i = 0; i < tout.size(); i++) {
        Tab t = tout.table(i);
        m_name_to_test_outputs_value_map[t.str(T_NAME)] = t.p;
    }
}

void FbsModel::clear_map()
{
    m_name_to_node_map.clear();
    m_name_to_initial_tensor_map.clear();
    m_name_to_value_info_map.clear();
    m_name_to_test_inputs_value_map.clear();
    m_name_to_test_outputs_value_map.clear();
}

void FbsModel::print()
{
    Tab graph = as_tab(m_model).table(MODEL_GRAPH);
    ESP_LOGI(TAG,
             "model %s: %u nodes, %u initializers, %u+%u test values",
             graph.str(GRAPH_NAME).c_str(),
             (unsigned)graph.vec(GRAPH_NODE).size(),
             (unsigned)graph.vec(GRAPH_INIT).size(),
             (unsigned)graph.vec(GRAPH_TEST_IN).size(),
             (unsigned)graph.vec(GRAPH_TEST_OUT).size());
}

/* Kahn's order, the earliest node in the file first among those ready: the
 * exporter writes them in order already, and then this is that order. */
std::vector<std::string> FbsModel::topological_sort()
{
    std::vector<std::string> order;
    Tab graph = as_tab(m_model).table(MODEL_GRAPH);
    Vec nodes = graph.vec(GRAPH_NODE);
    uint32_t n = nodes.size();
    std::map<std::string, uint32_t> producer;
    for (uint32_t i = 0; i < n; i++) {
        Vec outs = nodes.table(i).vec(NODE_OUTPUT);
        for (uint32_t k = 0; k < outs.size(); k++) producer[outs.str(k)] = i;
    }
    std::vector<int> pending(n, 0);
    std::vector<std::vector<uint32_t>> users(n);
    for (uint32_t i = 0; i < n; i++) {
        Vec ins = nodes.table(i).vec(NODE_INPUT);
        for (uint32_t k = 0; k < ins.size(); k++) {
            auto it = producer.find(ins.str(k));
            if (it == producer.end() || it->second == i) continue;
            pending[i]++;
            users[it->second].push_back(i);
        }
    }
    std::set<uint32_t> ready;
    for (uint32_t i = 0; i < n; i++)
        if (!pending[i]) ready.insert(i);
    while (!ready.empty()) {
        uint32_t i = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(nodes.table(i).str(NODE_NAME));
        for (uint32_t u : users[i])
            if (--pending[u] == 0) ready.insert(u);
    }
    if (order.size() != n) ESP_LOGE(TAG, "The graph has a cycle: %u of %u nodes ordered", (unsigned)order.size(), (unsigned)n);
    return order;
}

/* ---- attributes ---------------------------------------------------------- */

esp_err_t FbsModel::get_operation_attribute(std::string node_name, std::string attribute_name, int &ret_value)
{
    Tab a = find_attr(as_tab(lookup(m_name_to_node_map, node_name)), attribute_name);
    const uint8_t *f = a.field(ATTR_I);
    if (!f) return ESP_FAIL;
    int64_t v;
    memcpy(&v, f, sizeof v);
    ret_value = (int)v;
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name, std::string attribute_name, float &ret_value)
{
    Tab a = find_attr(as_tab(lookup(m_name_to_node_map, node_name)), attribute_name);
    const uint8_t *f = a.field(ATTR_F);
    if (!f) return ESP_FAIL;
    memcpy(&ret_value, f, sizeof ret_value);
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name, std::string attribute_name, std::string &ret_value)
{
    Tab a = find_attr(as_tab(lookup(m_name_to_node_map, node_name)), attribute_name);
    Vec s = a.vec(ATTR_S);
    if (!s.p) return ESP_FAIL;
    ret_value.assign((const char *)s.data(), s.size());
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name, std::string attribute_name, std::vector<int> &ret_value)
{
    Tab a = find_attr(as_tab(lookup(m_name_to_node_map, node_name)), attribute_name);
    Vec v = a.vec(ATTR_INTS);
    if (!v.p) return ESP_FAIL;
    ret_value = ints_of(v);
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name,
                                            std::string attribute_name,
                                            std::vector<float> &ret_value)
{
    Tab a = find_attr(as_tab(lookup(m_name_to_node_map, node_name)), attribute_name);
    Vec v = a.vec(ATTR_FLOATS);
    if (!v.p) return ESP_FAIL;
    ret_value.resize(v.size());
    for (uint32_t i = 0; i < v.size(); i++) ret_value[i] = v.at<float>(i);
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name, std::string attribute_name, dl::quant_type_t &ret_value)
{
    std::string s;
    if (get_operation_attribute(node_name, attribute_name, s) != ESP_OK) return ESP_FAIL;
    if (s == "S8" || s == "SYMM_S8")
        ret_value = dl::QUANT_TYPE_SYMM_8BIT;
    else if (s == "S16" || s == "SYMM_S16")
        ret_value = dl::QUANT_TYPE_SYMM_16BIT;
    else if (s == "S32" || s == "SYMM_S32")
        ret_value = dl::QUANT_TYPE_SYMM_32BIT;
    else if (s == "F32")
        ret_value = dl::QUANT_TYPE_FLOAT32;
    else {
        ESP_LOGE(TAG, "The quant type(%s) is not support now", s.c_str());
        ret_value = dl::QUANT_TYPE_NONE;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name,
                                            std::string attribute_name,
                                            dl::activation_type_t &ret_value)
{
    std::string s;
    if (get_operation_attribute(node_name, attribute_name, s) != ESP_OK) return ESP_FAIL;
    if (s == "None" || s == "Linear")
        ret_value = dl::Linear;
    else if (s == "Relu")
        ret_value = dl::ReLU;
    else if (s == "LeakyRelu")
        ret_value = dl::LeakyReLU;
    else if (s == "PRelu")
        ret_value = dl::PReLU;
    else {
        ESP_LOGE(TAG, "The activation type(%s) is not support now", s.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name, std::string attribute_name, dl::resize_mode_t &ret_value)
{
    std::string s;
    if (get_operation_attribute(node_name, attribute_name, s) != ESP_OK) return ESP_FAIL;
    if (s == "nearest")
        ret_value = dl::RESIZE_NEAREST;
    else if (s == "linear")
        ret_value = dl::RESIZE_LINEAR;
    else if (s == "cubic")
        ret_value = dl::RESIZE_CUBIC;
    else {
        ESP_LOGE(TAG, "The resize mode(%s) is not support now", s.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_attribute(std::string node_name, std::string attribute_name, dl::TensorBase *&ret_value)
{
    if (ret_value) ESP_LOGW(TAG, "(%s) The TensorBase pointer is not null, it may cause a memory leak", __func__);
    Tab a = find_attr(as_tab(lookup(m_name_to_node_map, node_name)), attribute_name);
    Tab t = a.table(ATTR_T);
    if (!t) {
        ret_value = nullptr;
        return ESP_FAIL;
    }
    ret_value = make_tensor(t, m_param_copy, MALLOC_CAP_DEFAULT);
    return ret_value ? ESP_OK : ESP_FAIL;
}

/* ---- nodes ----------------------------------------------------------------- */

esp_err_t FbsModel::get_operation_input_shape(std::string node_name, int index, std::vector<int> &ret_value)
{
    Tab node = as_tab(lookup(m_name_to_node_map, node_name));
    Vec ins = node.vec(NODE_INPUT);
    if (!node || index < 0 || (uint32_t)index >= ins.size()) return ESP_FAIL;
    std::string name = ins.str(index);
    if (lookup(m_name_to_initial_tensor_map, name))
        ret_value = get_tensor_shape(name);
    else
        ret_value = get_value_info_shape(name);
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_output_shape(std::string node_name, int index, std::vector<int> &ret_value)
{
    Tab node = as_tab(lookup(m_name_to_node_map, node_name));
    Vec outs = node.vec(NODE_OUTPUT);
    if (!node || index < 0 || (uint32_t)index >= outs.size()) return ESP_FAIL;
    ret_value = get_value_info_shape(outs.str(index));
    return ESP_OK;
}

esp_err_t FbsModel::get_operation_inputs_and_outputs(std::string node_name,
                                                     std::vector<std::string> &inputs,
                                                     std::vector<std::string> &outputs)
{
    Tab node = as_tab(lookup(m_name_to_node_map, node_name));
    if (!node) {
        ESP_LOGE(TAG, "Unknown node name: %s", node_name.c_str());
        return ESP_FAIL;
    }
    Vec ins = node.vec(NODE_INPUT), outs = node.vec(NODE_OUTPUT);
    inputs.clear();
    outputs.clear();
    for (uint32_t i = 0; i < ins.size(); i++) inputs.push_back(ins.str(i));
    for (uint32_t i = 0; i < outs.size(); i++) outputs.push_back(outs.str(i));
    return ESP_OK;
}

std::string FbsModel::get_operation_type(std::string node_name)
{
    Tab node = as_tab(lookup(m_name_to_node_map, node_name));
    return node ? node.str(NODE_OP) : std::string();
}

dl::TensorBase *FbsModel::get_operation_parameter(std::string node_name, int index, uint32_t caps)
{
    Tab node = as_tab(lookup(m_name_to_node_map, node_name));
    Vec ins = node.vec(NODE_INPUT);
    if (!node || index < 0 || (uint32_t)index >= ins.size()) return nullptr;
    return get_parameter(ins.str(index), caps);
}

dl::TensorBase *FbsModel::get_parameter(std::string tensor_name, uint32_t caps)
{
    Tab t = as_tab(lookup(m_name_to_initial_tensor_map, tensor_name));
    if (!t) return nullptr;
    return make_tensor(t, m_param_copy, caps);
}

esp_err_t FbsModel::get_operation_lut_name(std::string node_name, std::string &lut_name, std::string attribute_name)
{
    if (get_operation_attribute(node_name, attribute_name, lut_name) != ESP_OK || lut_name.empty()) return ESP_FAIL;
    if (!lookup(m_name_to_initial_tensor_map, lut_name)) {
        ESP_LOGE(TAG, "LUT initializer %s referenced by node %s was not found.", lut_name.c_str(), node_name.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

dl::TensorBase *FbsModel::get_operation_lut(std::string node_name, uint32_t caps, std::string attribute_name)
{
    std::string lut_name;
    if (get_operation_lut_name(node_name, lut_name, attribute_name) != ESP_OK) return nullptr;
    return get_parameter(lut_name, caps);
}

bool FbsModel::is_parameter(std::string name)
{
    return lookup(m_name_to_initial_tensor_map, name) != nullptr;
}

/* ---- tensors and value infos ------------------------------------------------ */

const void *FbsModel::get_tensor_raw_data(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_initial_tensor_map, tensor_name));
    return t ? tensor_raw(t) : nullptr;
}

dl::dtype_t FbsModel::get_tensor_dtype(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_initial_tensor_map, tensor_name));
    return t ? to_dtype(t.scalar<int32_t>(T_DTYPE)) : dl::DATA_TYPE_UNDEFINED;
}

std::vector<int> FbsModel::get_tensor_shape(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_initial_tensor_map, tensor_name));
    return t ? tensor_shape(t) : std::vector<int>();
}

std::vector<int> FbsModel::get_tensor_exponents(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_initial_tensor_map, tensor_name));
    return t ? tensor_exponents(t) : std::vector<int>();
}

dl::dtype_t FbsModel::get_value_info_dtype(std::string var_name)
{
    Tab tts = value_tensor_type(as_tab(lookup(m_name_to_value_info_map, var_name)));
    return tts ? to_dtype(tts.scalar<int32_t>(TTS_ELEM)) : dl::DATA_TYPE_UNDEFINED;
}

std::vector<int> FbsModel::get_value_info_shape(std::string var_name)
{
    std::vector<int> shape;
    Tab tts = value_tensor_type(as_tab(lookup(m_name_to_value_info_map, var_name)));
    if (!tts) return shape;
    Vec dims = tts.table(TTS_SHAPE).vec(0);
    for (uint32_t i = 0; i < dims.size(); i++) {
        Tab dv = dims.table(i).table(DIM_VALUE);
        if (dv.scalar<int8_t>(DV_TYPE) != DV_KIND_VALUE) {
            ESP_LOGE(TAG, "The dim type don't be support now");
            return std::vector<int>();
        }
        shape.push_back((int)dv.scalar<int64_t>(DV_VALUE));
    }
    return shape;
}

int FbsModel::get_value_info_exponent(std::string var_name)
{
    Tab vi = as_tab(lookup(m_name_to_value_info_map, var_name));
    Vec exps = vi.vec(VI_EXPS);
    if (!exps.size()) {
        ESP_LOGE(TAG, "The exponents size of ValueInfo(%s) is 0", var_name.c_str());
        return 0;
    }
    if (exps.size() != 1)
        ESP_LOGW(TAG, "The exponents size(%u) of ValueInfo(%s) is not equal to 1", (unsigned)exps.size(),
                 var_name.c_str());
    return (int)exps.at<int64_t>(0);
}

/* ---- test vectors ------------------------------------------------------------- */

const void *FbsModel::get_test_input_tensor_raw_data(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_test_inputs_value_map, tensor_name));
    return t ? tensor_raw(t) : nullptr;
}

const void *FbsModel::get_test_output_tensor_raw_data(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_test_outputs_value_map, tensor_name));
    return t ? tensor_raw(t) : nullptr;
}

dl::TensorBase *FbsModel::get_test_input_tensor(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_test_inputs_value_map, tensor_name));
    if (!t) {
        ESP_LOGE(TAG, "The test input tensor(%s) is nullptr", tensor_name.c_str());
        return nullptr;
    }
    return make_tensor(t, true, MALLOC_CAP_DEFAULT);
}

dl::TensorBase *FbsModel::get_test_output_tensor(std::string tensor_name)
{
    Tab t = as_tab(lookup(m_name_to_test_outputs_value_map, tensor_name));
    if (!t) {
        ESP_LOGE(TAG, "The test output tensor(%s) is nullptr", tensor_name.c_str());
        return nullptr;
    }
    return make_tensor(t, true, MALLOC_CAP_DEFAULT);
}

std::vector<std::string> FbsModel::get_test_outputs_name()
{
    std::vector<std::string> names;
    Vec tout = as_tab(m_model).table(MODEL_GRAPH).vec(GRAPH_TEST_OUT);
    for (uint32_t i = 0; i < tout.size(); i++) names.push_back(tout.table(i).str(T_NAME));
    return names;
}

/* ---- the graph and the model ---------------------------------------------------- */

std::vector<std::string> FbsModel::get_graph_inputs()
{
    std::vector<std::string> names;
    Tab graph = as_tab(m_model).table(MODEL_GRAPH);
    std::set<std::string> inits;
    Vec iv = graph.vec(GRAPH_INIT);
    for (uint32_t i = 0; i < iv.size(); i++) inits.insert(iv.table(i).str(T_NAME));
    Vec in = graph.vec(GRAPH_INPUT);
    for (uint32_t i = 0; i < in.size(); i++) {
        std::string n = in.table(i).str(VI_NAME);
        if (!inits.count(n)) names.push_back(n);
    }
    return names;
}

std::vector<std::string> FbsModel::get_graph_outputs()
{
    std::vector<std::string> names;
    Vec out = as_tab(m_model).table(MODEL_GRAPH).vec(GRAPH_OUTPUT);
    for (uint32_t i = 0; i < out.size(); i++) names.push_back(out.table(i).str(VI_NAME));
    return names;
}

std::string FbsModel::get_model_name()
{
    return as_tab(m_model).table(MODEL_GRAPH).str(GRAPH_NAME);
}

int64_t FbsModel::get_model_version()
{
    return as_tab(m_model).scalar<int64_t>(MODEL_VERSION);
}

std::string FbsModel::get_model_doc_string()
{
    return as_tab(m_model).str(MODEL_DOC);
}

std::string FbsModel::get_model_metadata_prop(const std::string &key)
{
    Vec props = as_tab(m_model).vec(MODEL_META);
    for (uint32_t i = 0; i < props.size(); i++) {
        Tab kv = props.table(i);
        if (kv.str(KV_KEY) == key) return kv.str(KV_VALUE);
    }
    return std::string();
}

void FbsModel::get_model_size(size_t *internal_size, size_t *psram_size, size_t *psram_rodata_size, size_t *flash_size)
{
    /* the model is a buffer of the app's in PSRAM (or, in the simulator, memory) */
    if (internal_size) *internal_size = 0;
    if (psram_size) *psram_size = m_size;
    if (psram_rodata_size) *psram_rodata_size = 0;
    if (flash_size) *flash_size = 0;
}

} // namespace fbs

#if VE_UNPACK_FILTERS
/* ---- the simulator: the SIMD filters back to the C kernels' layout ----------
 *
 * esp-ppq exports a model for one target. For the P4 it packs every
 * convolution's filter (Conv, Gemm) in blocks of the SIMD's lanes: 16 output
 * channels for 8-bit weights, 8 otherwise, each block (N/a) H W C (a), the
 * channels left over after the last block plain (N H W C) for a convolution
 * and C H W N for a depthwise one. esp-dl's C kernels, the simulator's, read
 * N H W C and, depthwise, H W C N (esp-ppq's ResetParamLayoutPattern, in
 * parser/espdl/export_patterns.py, Apache-2.0). This undoes the packing in a
 * copy of the model, before esp-dl reads it. */
static void unpack_one(uint8_t *data, int n, int h, int w, int c, int align, bool depthwise, size_t es)
{
    size_t total = (size_t)n * h * w * c * es;
    std::vector<uint8_t> orig(total);  /* N H W C */
    auto at = [&](int nn, int hh, int ww, int cc) { return &orig[((((size_t)nn * h + hh) * w + ww) * c + cc) * es]; };
    const uint8_t *src = data;
    int n_al = n / align * align;
    for (int b = 0; b < n_al / align; b++)
        for (int hh = 0; hh < h; hh++)
            for (int ww = 0; ww < w; ww++)
                for (int cc = 0; cc < c; cc++)
                    for (int a = 0; a < align; a++, src += es) memcpy(at(b * align + a, hh, ww, cc), src, es);
    if (!depthwise) {
        for (int nn = n_al; nn < n; nn++)
            for (int hh = 0; hh < h; hh++)
                for (int ww = 0; ww < w; ww++)
                    for (int cc = 0; cc < c; cc++, src += es) memcpy(at(nn, hh, ww, cc), src, es);
    } else {
        for (int cc = 0; cc < c; cc++)
            for (int hh = 0; hh < h; hh++)
                for (int ww = 0; ww < w; ww++)
                    for (int nn = n_al; nn < n; nn++, src += es) memcpy(at(nn, hh, ww, cc), src, es);
    }
    uint8_t *dst = data;
    if (!depthwise) {
        memcpy(dst, orig.data(), total);
    } else {
        for (int hh = 0; hh < h; hh++)
            for (int ww = 0; ww < w; ww++)
                for (int cc = 0; cc < c; cc++)
                    for (int nn = 0; nn < n; nn++, dst += es) memcpy(dst, at(nn, hh, ww, cc), es);
    }
}

namespace fbs {

int unpack_simd_filters(uint8_t *flatbuffer)
{
    Tab graph = as_tab(flatbuffer + rd32(flatbuffer)).table(MODEL_GRAPH);
    std::map<std::string, Tab> inits;
    Vec iv = graph.vec(GRAPH_INIT);
    for (uint32_t i = 0; i < iv.size(); i++) inits[iv.table(i).str(T_NAME)] = iv.table(i);
    std::set<std::string> done;
    int count = 0;
    Vec nodes = graph.vec(GRAPH_NODE);
    for (uint32_t i = 0; i < nodes.size(); i++) {
        Tab node = nodes.table(i);
        std::string op = node.str(NODE_OP);
        if (op != "Conv" && op != "Gemm" && op != "MatMul") continue;
        Vec ins = node.vec(NODE_INPUT);
        if (ins.size() < 2) continue;
        auto it = inits.find(ins.str(1));
        if (it == inits.end() || done.count(it->first)) continue;
        Tab t = it->second;
        std::string qt;
        Vec qs = find_attr(node, "quant_type").vec(ATTR_S);
        if (qs.p) qt.assign((const char *)qs.data(), qs.size());
        if (qt.empty() || qt == "F32") continue;
        const uint8_t *g = find_attr(node, "group").field(ATTR_I);
        int64_t group = 1;
        if (g) memcpy(&group, g, sizeof group);
        std::vector<int> dims = tensor_shape(t);
        int dt = t.scalar<int32_t>(T_DTYPE);
        size_t es = (dt == dl::DATA_TYPE_INT16 || dt == dl::DATA_TYPE_UINT16) ? 2 : 1;
        Vec raw = t.vec(T_RAW);
        if (!raw.p) continue;
        int align = (qt == "S8" || qt == "SYMM_S8") ? 16 : 8;
        bool dw = group > 1;
        int h, w, c, n;
        if (op == "MatMul") {
            /* a constant right-hand side, [.., C, N]: each C x N slice packed as a 1x1 filter */
            if (dims.size() < 2) continue;
            c = dims[dims.size() - 2];
            n = dims[dims.size() - 1];
            size_t slices = 1;
            for (size_t k = 0; k + 2 < dims.size(); k++) slices *= (size_t)dims[k];
            if ((size_t)raw.size() * 16 < slices * n * c * es) continue;
            for (size_t k = 0; k < slices; k++)
                unpack_one((uint8_t *)raw.data() + k * n * c * es, n, 1, 1, c, align, false, es);
            done.insert(it->first);
            count++;
            continue;
        }
        if (dims.size() == 4) {
            h = dims[0];
            w = dims[1];
            c = dw ? dims[3] : dims[2];
            n = dw ? dims[2] : dims[3];
        } else if (dims.size() == 3) {
            h = 1;
            w = dims[0];
            c = dw ? dims[2] : dims[1];
            n = dw ? dims[1] : dims[2];
        } else {
            continue;
        }
        if ((size_t)raw.size() * 16 < (size_t)n * h * w * c * es) continue;
        unpack_one((uint8_t *)raw.data(), n, h, w, c, align, dw, es);
        done.insert(it->first);
        count++;
        /* An 8-bit convolution quantized per channel: the C kernels' tails
         * (dl_c_s8_tail_bias_per_ch_*) add a 16-bit bias to the sum shifted
         * right by 4, where the SIMD's take the 32-bit one esp-ppq exports
         * for the P4: written over it, at the start of its own buffer. */
        if (align == 16 && tensor_exponents(t).size() > 1 && ins.size() > 2) {
            auto bt = inits.find(ins.str(2));
            if (bt == inits.end() || done.count(bt->first) || bt->second.scalar<int32_t>(T_DTYPE) != dl::DATA_TYPE_INT32)
                continue;
            Vec braw = bt->second.vec(T_RAW);
            if (!braw.p) continue;
            int32_t *b32 = (int32_t *)braw.data();
            int16_t *b16 = (int16_t *)braw.data();
            for (int k = 0; k < n; k++) {
                int32_t v = b32[k] >> 4;
                b16[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            done.insert(bt->first);
        }
    }
    return count;
}

} // namespace fbs
#endif
