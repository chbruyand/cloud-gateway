/*
 * This file is part of Nuage Labs SAS's Cloud Gateway.
 *
 * Copyright (C) 2011-2017  Nuage Labs SAS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#ifndef CLOUD_UTILS_EVENT_INTERNAL_H
#define CLOUD_UTILS_EVENT_INTERNAL_H

#include <event2/event.h>

#include "cloudutils/cloudutils_event.h"

COMPILER_BLOCK_VISIBILITY_DEFAULT

COMPILER_PURE_FUNCTION struct event_base * cgutils_event_get_base(cgutils_event_data * data);

COMPILER_BLOCK_VISIBILITY_END

#endif /* CLOUD_UTILS_EVENT_INTERNAL_H */
