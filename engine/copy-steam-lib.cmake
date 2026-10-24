# Puts the newest libsteam_api.so steamworks-sys built next to niri-island-steam
# (it finds it there through its rpath). Run with -DOUT=<cargo release dir>.
file(GLOB libs "${OUT}/build/steamworks-sys-*/out/libsteam_api.so")
set(newest "")
foreach(lib ${libs})
    if(newest STREQUAL "" OR lib IS_NEWER_THAN newest)
        set(newest ${lib})
    endif()
endforeach()
if(newest)
    file(COPY_FILE ${newest} ${OUT}/libsteam_api.so ONLY_IF_DIFFERENT)
endif()
