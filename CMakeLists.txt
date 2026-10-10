# ESP-IDF only. stm-forge never reads this file: it compiles every src/*.c of
# the library and adds include/, which is why the sources sit flat in src/ and
# each port file is empty on the other platform.
#
# The core is always built, even with CONFIG_COMPASS off; each chip driver
# only when its Kconfig option is on. Kconfig values are available here during
# the build phase, and only SRCS depends on them -- never REQUIRES, which IDF
# resolves before sdkconfig. That is also why COMPASS off still builds the
# core: an empty SRCS would need a second, differently-shaped register call,
# and an unreferenced core costs nothing once --gc-sections has run.
set(srcs "src/compass.c" "src/compass_log.c" "src/compass_port_esp.c")

if(CONFIG_COMPASS AND NOT (CONFIG_COMPASS_QMC5883L OR CONFIG_COMPASS_QMC5883P
                           OR CONFIG_COMPASS_IST8310 OR CONFIG_COMPASS_LIS3MDL))
    message(WARNING "compass: CONFIG_COMPASS is on but no chip driver is "
                    "selected; set CONFIG_COMPASS_<CHIP>=y "
                    "(esp-forge: COMPASS_<CHIP> = y)")
endif()

if(CONFIG_COMPASS_QMC5883L)
    list(APPEND srcs "src/qmc5883l.c")
endif()
if(CONFIG_COMPASS_QMC5883P)
    list(APPEND srcs "src/qmc5883p.c")
endif()
if(CONFIG_COMPASS_IST8310)
    list(APPEND srcs "src/ist8310.c")
endif()
if(CONFIG_COMPASS_LIS3MDL)
    list(APPEND srcs "src/lis3mdl.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES driver
                       PRIV_REQUIRES esp_timer)
