# Layer state only exists on the central, so there is nothing to record or
# restore anywhere else.
if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
 target_sources_ifdef(CONFIG_ZMK_PERSISTENT_LAYERS app PRIVATE src/persistent_layers.c)
endif()
