#######################################
# Effect

qt5_add_dbus_adaptor( kwin4_effect_builtins_sources projector/org.kde.kwin.ProjectorEffect.xml projector/projector.h KWin::ProjectorEffect )

# Source files
set( kwin4_effect_builtins_sources ${kwin4_effect_builtins_sources}
    projector/projector.cpp
    )
