#pragma once

#include "core/IUsbRequestHandler.h"
#include <cstdio>

class StubHandler : public IUsbRequestHandler {
public:
    void handle(const UsbRequest& req, UsbResponse& res) override;
    void onDetach() override;

private:
    int request_count = 0;
};