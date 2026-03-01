#include "StubHandler.h"

void StubHandler::handle(const UsbRequest& req, UsbResponse& res) {
    request_count++;
    printf("[StubHandler] request #%d — endpoint: 0x%02X, urb_type: 0x%02X, data_len: %zu\n",
        request_count, req.endpoint, req.urb_type, req.data.size());

    res.status = 0;
    res.data.clear();
}

void StubHandler::onDetach() {
    printf("[StubHandler] device detached — total requests handled: %d\n", request_count);
}
