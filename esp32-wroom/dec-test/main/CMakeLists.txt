idf_component_register(
                    SRCS "i2s.c" "buf.c" "main.c" "dec.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                    esp_audio_codec
                    esp_driver_i2s
                    esp_ringbuf
                    EMBED_FILES "test_5s.mp3"
                    )
