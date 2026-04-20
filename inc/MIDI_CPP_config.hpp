#ifndef MIDI_CPP_CONFIG_H
#define MIDI_CPP_CONFIG_H

/*
  Local host-side defaults for building against MIDI_CPP from this utility.
  These values are only used to satisfy library configuration on desktop builds.
*/

#ifndef SYX_PRODUCT_ID_LSB
#define SYX_PRODUCT_ID_LSB 0x00
#endif

#ifndef SYX_ID_BL_VER1
#define SYX_ID_BL_VER1 0
#endif
#ifndef SYX_ID_BL_VER2
#define SYX_ID_BL_VER2 0
#endif
#ifndef SYX_ID_BL_VER3
#define SYX_ID_BL_VER3 0
#endif

#ifndef SYX_ID_APP_VER1
#define SYX_ID_APP_VER1 0
#endif
#ifndef SYX_ID_APP_VER2
#define SYX_ID_APP_VER2 0
#endif
#ifndef SYX_ID_APP_VER3
#define SYX_ID_APP_VER3 0
#endif

#ifndef SYX_SEND_RETURN_CODE_OK
#define SYX_SEND_RETURN_CODE_OK 0
#endif
#ifndef SYX_SEND_RETURN_CODE_NO_SEND_FUNCTION
#define SYX_SEND_RETURN_CODE_NO_SEND_FUNCTION -1
#endif
#ifndef SYX_SEND_RETURN_CODE_ERROR
#define SYX_SEND_RETURN_CODE_ERROR -2
#endif

#ifndef SYX_TX_BLOCK_SIZE
#define SYX_TX_BLOCK_SIZE 48
#endif
#ifndef SYX_RX_BLOCK_SIZE
#define SYX_RX_BLOCK_SIZE 64
#endif

#ifndef SYX_FORMAT_KBP4_EDITOR_MESSAGE
#define SYX_FORMAT_KBP4_EDITOR_MESSAGE SYX_FORMAT_DANS_EDITOR_MESSAGE
#endif

#endif
