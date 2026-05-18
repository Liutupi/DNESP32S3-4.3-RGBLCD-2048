set(src_dirs
            LED
            MYIIC
            XL9555
            RGBLCD
            TOUCH
            SPI
            SDIO)

set(include_dirs
            LED
            MYIIC
            XL9555
            RGBLCD
            TOUCH
            SPI
            SDIO)

set(requires
            driver
            esp_lcd
            fatfs
            sdmmc)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)