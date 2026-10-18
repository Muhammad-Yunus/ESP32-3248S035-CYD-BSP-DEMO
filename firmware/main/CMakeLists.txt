idf_component_register(SRCS "firmware.c" "file_explorer.c" "mp3_player.c"
                    INCLUDE_DIRS "."
                    REQUIRES bsp_esp323248s035 fatfs)
