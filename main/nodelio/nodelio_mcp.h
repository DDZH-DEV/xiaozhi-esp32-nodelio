#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Register self.nodelio.* MCP tools（宜在 AddCommonTools 之前调用，以便紧随 common）. */
void nodelio_mcp_register_tools(void);

#ifdef __cplusplus
}
#endif
