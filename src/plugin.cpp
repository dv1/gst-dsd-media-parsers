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

#include <config.h>

#include <gst/gst.h>
#include "gstdffparse.hpp"
#include "gstdsfparse.hpp"


GST_DEBUG_CATEGORY(dsdchunkparse_debug);


extern "C" {

static gboolean plugin_init(GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT(dsdchunkparse_debug, "dsdchunkparse", 0, "DSD chunk-based media parse base class");

	gboolean ret = TRUE;
	ret |= GST_ELEMENT_REGISTER(dffparse, plugin);
	ret |= GST_ELEMENT_REGISTER(dsfparse, plugin);
	ret |= gst_type_find_register(plugin, GST_DFF_MEDIA_TYPE, GST_RANK_PRIMARY, gst_dffparse_type_find, "dff", nullptr, nullptr, nullptr);
	ret |= gst_type_find_register(plugin, GST_DSF_MEDIA_TYPE, GST_RANK_PRIMARY, gst_dsfparse_type_find, "dsf", nullptr, nullptr, nullptr);

	return ret;
}

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	dsdmediaparsers,
	"elements for parsing DSD container formats",
	plugin_init,
	VERSION,
	"LGPL",
	GST_PACKAGE_NAME,
	GST_PACKAGE_ORIGIN
)

}
