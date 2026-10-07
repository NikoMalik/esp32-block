// web dashboard + control endpoints + remote blocklist auto-update
#pragma once

void web_begin();   // load update cfg, register routes, start server
void web_tick();    // handleClient + periodic remote blocklist fetch
