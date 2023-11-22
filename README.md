# BambuStudioWebHookProxy

A dynamic library that proxies the bambu_networking library that is part of the closed source Bambu Studio network plugin.

All calls are passed through unmodified with the exception of bambu_network_set_on_local_message_fn, which uses an interceptor function to invoke a configured webhook url with the json message status from the printer.

This is necessary to work around an issue with the Bambu P1 LAN mode where only a single concurrent MQTT connection is functional.

Note: it appears that Bambu Labs does not maintain backwards compatibility with the method signatures expected between versions of Bambu Studio and the network plugins - so this plugin must be matched to the version of the library in use or it will likely break/segfault.

Dependencies from the BambuStudio src: BambuStudio/src/slic3r/Utils/bambu_networking.hpp BambuStudio/src/libslic3r/ProjectTask.hpp

Function signatures derived from BambuStudio/src/slic3r/Utils/NetworkAgent.hpp


## Supported OS

This is currently supported on macOS and Linux.  (Specifically tested on macOS 14 and Debian 12). 

I don't run Windows, and don't have a way to build out Windows support (much less personal need).  I'd be happy to take a PR.  

## Where is it? 

Select the branch matching the version of the Bambu Studio network plugin.


## buy me a beer

If you find this useful and feel so inclinced, https://paypal.me/slynn1324.  Otherwise, simply enjoy. 
