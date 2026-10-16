# Signing in with OAuth 2 in a browser (ADR 0019), for the app and the CLI.
# QtNetworkAuth stays out of libe3core, which renews tokens on its own.
add_library(e3oauthflow STATIC SignIn.cpp SignIn.h)
target_include_directories(e3oauthflow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(e3oauthflow PUBLIC e3core Qt6::NetworkAuth)
set_target_properties(e3oauthflow PROPERTIES AUTOMOC ON)
e3mail_set_warnings(e3oauthflow)
