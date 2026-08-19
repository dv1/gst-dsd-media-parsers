/* GStreamer DSD container formats parser plugin
 * Copyright (C) 2026 Carlos Rafael Giani <crg7475@mailbox.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <gst/gst.h>
#include "gstdsdchunkparse.hpp"


#define GST_DFF_MEDIA_TYPE "audio/x-dff"

#define GST_TYPE_DFFPARSE (gst_dffparse_get_type())
G_DECLARE_FINAL_TYPE(GstDFFParse, gst_dffparse, GST, DFFPARSE, GstDsdChunkParse)
#define GST_DFFPARSE_CAST(obj) ((GstDFFParse *)(obj))

GST_ELEMENT_REGISTER_DECLARE(dffparse);

void gst_dffparse_type_find(GstTypeFind *tf, gpointer);
