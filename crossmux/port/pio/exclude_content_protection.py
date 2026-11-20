"""Skip the device-side content-protection binding on the host port.

The simulator links firmware_link_stubs.cpp, which supplies
freeink::content::openProtectedBook(); lib/Epub/ContentProtection.cpp is the
wolfSSL/SD-backed implementation and must not be compiled here.
"""

Import("env")


def skip_device_binding(node):
    return None


env.AddBuildMiddleware(skip_device_binding, "*lib/Epub/ContentProtection.cpp")
