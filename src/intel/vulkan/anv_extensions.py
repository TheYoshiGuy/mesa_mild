COPYRIGHT = """\
/*
 * Copyright 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import xml.etree.cElementTree as et

from mako.template import Template

class Extension:
    def __init__(self, name, ext_version, enable):
        self.name = name
        self.ext_version = int(ext_version)
        if enable is True:
            self.enable = 'true';
        elif enable is False:
            self.enable = 'false';
        else:
            self.enable = enable;

EXTENSIONS = [
    Extension('VK_KHR_dedicated_allocation',              1, True),
    Extension('VK_KHR_descriptor_update_template',        1, True),
    Extension('VK_KHR_external_memory',                   1, True),
    Extension('VK_KHR_external_memory_capabilities',      1, True),
    Extension('VK_KHR_external_memory_fd',                1, True),
    Extension('VK_KHR_get_memory_requirements2',          1, True),
    Extension('VK_KHR_get_physical_device_properties2',   1, True),
    Extension('VK_KHR_get_surface_capabilities2',         1, True),
    Extension('VK_KHR_incremental_present',               1, True),
    Extension('VK_KHR_maintenance1',                      1, True),
    Extension('VK_KHR_push_descriptor',                   1, True),
    Extension('VK_KHR_sampler_mirror_clamp_to_edge',      1, True),
    Extension('VK_KHR_shader_draw_parameters',            1, True),
    Extension('VK_KHR_storage_buffer_storage_class',      1, True),
    Extension('VK_KHR_surface',                          25, True),
    Extension('VK_KHR_swapchain',                        68, True),
    Extension('VK_KHR_variable_pointers',                 1, True),
    Extension('VK_KHR_wayland_surface',                   6, 'VK_USE_PLATFORM_WAYLAND_KHR'),
    Extension('VK_KHR_xcb_surface',                       6, 'VK_USE_PLATFORM_XCB_KHR'),
    Extension('VK_KHR_xlib_surface',                      6, 'VK_USE_PLATFORM_XLIB_KHR'),
    Extension('VK_KHX_multiview',                         1, True),
]

def init_exts_from_xml(xml):
    """ Walk the Vulkan XML and fill out extra extension information. """

    xml = et.parse(xml)

    ext_name_map = {}
    for ext in EXTENSIONS:
        ext_name_map[ext.name] = ext

    for ext_elem in xml.findall('.extensions/extension'):
        ext_name = ext_elem.attrib['name']
        if ext_name not in ext_name_map:
            continue
        ext = ext_name_map[ext_name]

        ext.type = ext_elem.attrib['type']

    for ext in EXTENSIONS:
        assert ext.type == 'instance' or ext.type == 'device'

TEMPLATE = Template(COPYRIGHT + """
#include "anv_private.h"

#include "vk_util.h"

/* Convert the VK_USE_PLATFORM_* defines to booleans */
%for platform in ['ANDROID', 'WAYLAND', 'XCB', 'XLIB']:
#ifdef VK_USE_PLATFORM_${platform}_KHR
#   undef VK_USE_PLATFORM_${platform}_KHR
#   define VK_USE_PLATFORM_${platform}_KHR true
#else
#   define VK_USE_PLATFORM_${platform}_KHR false
#endif
%endfor

bool
anv_instance_extension_supported(const char *name)
{
%for ext in instance_extensions:
    if (strcmp(name, "${ext.name}") == 0)
        return ${ext.enable};
%endfor
    return false;
}

VkResult anv_EnumerateInstanceExtensionProperties(
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
    VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

%for ext in instance_extensions:
    if (${ext.enable}) {
        vk_outarray_append(&out, prop) {
            *prop = (VkExtensionProperties) {
                .extensionName = "${ext.name}",
                .specVersion = ${ext.ext_version},
            };
        }
    }
%endfor

    return vk_outarray_status(&out);
}

bool
anv_physical_device_extension_supported(struct anv_physical_device *device,
                                        const char *name)
{
%for ext in device_extensions:
    if (strcmp(name, "${ext.name}") == 0)
        return ${ext.enable};
%endfor
    return false;
}

VkResult anv_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice                            physicalDevice,
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
    ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
    VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);
    (void)device;

%for ext in device_extensions:
    if (${ext.enable}) {
        vk_outarray_append(&out, prop) {
            *prop = (VkExtensionProperties) {
                .extensionName = "${ext.name}",
                .specVersion = ${ext.ext_version},
            };
        }
    }
%endfor

    return vk_outarray_status(&out);
}
""")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', help='Output C file.', required=True)
    parser.add_argument('--xml', help='Vulkan API XML file.', required=True)
    args = parser.parse_args()

    init_exts_from_xml(args.xml)

    template_env = {
        'instance_extensions': [e for e in EXTENSIONS if e.type == 'instance'],
        'device_extensions': [e for e in EXTENSIONS if e.type == 'device'],
    }

    with open(args.out, 'w') as f:
        f.write(TEMPLATE.render(**template_env))
