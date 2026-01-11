/**
 * Copyright © 2025 NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <systemd/sd-bus.h>

/* Initialize the archive D-Bus service */
int archive_service_init(sd_bus **bus);

/* Run the archive service event loop
 * Returns 0 on success, -1 on error
 * Checks *should_exit periodically - if set to non-zero, returns 0 */
int archive_service_run(sd_bus *bus, const volatile sig_atomic_t *should_exit);

/* Cleanup the archive D-Bus service */
void archive_service_fini(sd_bus *bus);
