idf_component_register(SRC_DIRS "." INCLUDE_DIRS ".")
# esp_async_fbcpy (DMA2D between frame buffers) is esp_lcd's private API:
# the DPI driver only copies into the buffer being scanned, which tears.
target_include_directories(${COMPONENT_LIB} PRIVATE "$ENV{IDF_PATH}/components/esp_lcd/priv_include")
