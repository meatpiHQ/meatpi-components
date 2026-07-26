idf_component_register(
    SRCS "usb_eth_host.c"
         "usb_eth_netif_glue.c"
         "usb_eth_rtl8152.c"
         "usb_eth_asix.c"
         "usb_eth_cdc_ecm.c"
         "usb_eth_cdc_ncm.c"
         "usb_eth_rndis.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Driver layer (adopted field-proven legacy, ADAPTED for the
    # vendored cherryusb fork): CherryUSB host bring-up + ONE esp_netif
    # per active eth class (u0..u4), DHCP/static, subnet-overlap
    # suspension vs the WiFi STA. Policy/settings live in
    # usb_host_manager. NOTE: cherryusb's own cmake already excludes
    # the platform usbh_net.c glue whose strong symbols would bypass
    # the hooks here (see cherryusb/PROVENANCE.md).
    REQUIRES esp_netif
    PRIV_REQUIRES cherryusb esp_event lwip esp_driver_gpio freertos
)
