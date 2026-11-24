"""Supply headers omitted by the pinned Infratic release under Arduino-ESP32 3."""
Import("env")


def include_missing_header(build_env, source):
    header = "string" if source.name == "base58.cpp" else "WiFiClient.h"
    return build_env.Object(source, CXXFLAGS=build_env["CXXFLAGS"] + ["-include", header])


env.AddBuildMiddleware(include_missing_header, "*/Infratic/src/base58.cpp")
env.AddBuildMiddleware(include_missing_header, "*/Infratic/src/Infratic-ws.cpp")
