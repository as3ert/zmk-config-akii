# Out-of-tree driver sources for the AKII keyboard. Only compiled when the
# two-tier kscan node is present and its Kconfig symbol is enabled.
if(CONFIG_ZMK_KSCAN_AKII_TWOTIER)
  zephyr_library()
  zephyr_library_sources(drivers/kscan/kscan_akii_twotier.c)
endif()
