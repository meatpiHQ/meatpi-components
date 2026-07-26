idf_component_register(
    SRCS "mdns_manager.c"
         "mdns_manager_policy.c"
         "mdns_manager_settings.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: the mDNS advertisement (legacy HA-discovery contract:
    # _wican._tcp:80, wican_<id> hostname, mac/device_id TXT). esp-mdns
    # is a MANAGED component (see idf_component.yml).
    PRIV_REQUIRES espressif__mdns dev_status_manager settings_manager
                  log_manager
)
