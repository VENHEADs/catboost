LIBRARY()



SRCS(
    mappers.cpp
    master.cpp
    worker.cpp
)

PEERDIR(
    catboost/libs/algo
    catboost/libs/helpers
    catboost/libs/options
    library/binsaver
    library/containers/2d_array
    library/par
)

END()
