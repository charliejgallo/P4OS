/*
 * P4OS - the 802.15.4 radio's link to the C6 on the board: esp_hosted's
 * peer_data channel (aos_154_proto.h). A C6 whose firmware does not have the
 * channel rejects what is sent, and nothing comes back.
 */
#include "aos_154.h"
#include "aos_154_proto.h"
#include "aos_hal.h"
#include "eh_host_feat_peer_data.h"

static void on_msg(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    aos_154_on_msg(id, data, len);
}

bool aos_154_link_open(void)
{
    static const uint32_t IDS[] = { AOS_154_MSG_STATUS, AOS_154_MSG_SCAN, AOS_154_MSG_FRAMES, AOS_154_MSG_LOG,
                                    AOS_154_MSG_SLOTS, AOS_154_MSG_SPINEL };
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++)
        if (eh_host_peer_data_register(IDS[i], on_msg, NULL) != ESP_OK) {
            aos_hal_log("154", "could not register with esp_hosted's peer_data");
            return false;
        }
    return true;
}

bool aos_154_link_send(const uint8_t *data, size_t len)
{
    esp_err_t e = eh_host_peer_data_send(AOS_154_MSG_CMD, data, len);
    if (e != ESP_OK) aos_hal_log("154", "the C6 did not take the request (%s)", esp_err_to_name(e));
    return e == ESP_OK;
}

bool aos_154_link_spinel(const uint8_t *data, size_t len)
{
    return eh_host_peer_data_send(AOS_154_MSG_SPINEL_IN, data, len) == ESP_OK;
}
