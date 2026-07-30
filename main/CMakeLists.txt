idf_component_register(SRCS  "main.cpp"
                    INCLUDE_DIRS "." )
# Jet requires C++17
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

#idf_component_register(SRCS main.cpp)
