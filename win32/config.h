#ifdef _WIN32
    #ifdef SOLV_STATIC_LIB
        #define SOLV_API
    #else
        #ifdef SOLV_EXPORTS
            #define SOLV_API __declspec(dllexport)
        #else
            #define SOLV_API __declspec(dllimport)
        #endif
    #endif
#else
    #define SOLV_API
#endif