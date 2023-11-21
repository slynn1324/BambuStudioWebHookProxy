# BSWHP - Bambu Studio Webhook Proxy

A dynamic library that proxies the bambu_networking library that is part of the closed source Bambu Studio network plugin.  

All calls are passed through unmodified with the exception of `bambu_network_set_on_local_message_fn`, which uses an interceptor
function to invoke a configured webhook url with the json message status from the printer.

This is necessary to work around an issue with the Bambu P1 LAN mode where only a single concurrent MQTT connection is functional.

Note: it appears that Bambu Labs does not maintain backwards compatibility with the method signatures expected between versions
of Bambu Studio and the network plugins - so this plugin must be matched to the version of the library in use or it will likely break/segfault.
 
Dependencies from the BambuStudio src:
  BambuStudio/src/slic3r/Utils/bambu_networking.hpp
  BambuStudio/src/libslic3r/ProjectTask.hpp
  
Function signatures derived from BambuStudio/src/slic3r/Utils/NetworkAgent.hpp


## building

See build-mac.sh


## configuration

Create the following files:

macOS:

```
~/Library/Application Support/BambuStudioBeta/bswhp_url.txt
	A single line with the URL you want messages posted to.

~/Library/Application Support/BambuStudioBeta/bsqhp_debug.txt
	A single line with 0 = no debug logging, 1 = normal debug logging, 2 = verbose debug logging

```



