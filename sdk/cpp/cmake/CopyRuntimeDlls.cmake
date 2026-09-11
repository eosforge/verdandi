# 保留列表为单个 -D 参数；静态依赖组合没有运行时 DLL，是合法的空操作。
if(VERDANDI_RUNTIME_DLLS)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${VERDANDI_RUNTIME_DLLS} "${VERDANDI_RUNTIME_DIRECTORY}"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
