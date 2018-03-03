set_global_assignment -entity "mpll" -library "lib_mpll" -name IP_TOOL_NAME "altera_pll"
set_global_assignment -entity "mpll" -library "lib_mpll" -name IP_TOOL_VERSION "17.0"
set_global_assignment -entity "mpll" -library "lib_mpll" -name IP_TOOL_ENV "mwpim"
set_global_assignment -library "lib_mpll" -name SPD_FILE [file join $::quartus(sip_path) "mpll.spd"]

set_global_assignment -library "lib_mpll" -name MISC_FILE [file join $::quartus(sip_path) "mpll_sim/mpll.vo"]
