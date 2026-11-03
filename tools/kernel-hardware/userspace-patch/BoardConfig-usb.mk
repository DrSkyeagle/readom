# Set PW3_USB_COMPONENT_ROOT in BoardConfig before including this fragment.
ifndef PW3_USB_COMPONENT_ROOT
$(error Set PW3_USB_COMPONENT_ROOT to the verified public USB component)
endif
BOARD_PW3_USB_ONLY_ADBD := true
BOARD_SEPOLICY_DIRS += $(PW3_USB_COMPONENT_ROOT)/policy
