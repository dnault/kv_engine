/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "durability_monitor.h"

std::string DurabilityMonitor::to_string(Tracking tracking) {
    auto value = std::to_string(static_cast<uint8_t>(tracking));
    switch (tracking) {
    case Tracking::Memory:
        return value + ":memory";
    case Tracking::Disk:
        return value + ":disk";
    };
    return value + ":invalid";
}

std::ostream& operator<<(std::ostream& os, const DurabilityMonitor& dm) {
    dm.toOStream(os);
    return os;
}