

PYTEST()

SIZE(LARGE)

TAG(
    ya:force_sandbox
    sb:intel_e5_2660v1
    ya:fat
)

TEST_SRCS(
    main.py
)

DEPENDS(
    library/string_utils/base64/bench
)

END()

NEED_CHECK()
