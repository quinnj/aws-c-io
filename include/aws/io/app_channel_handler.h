#ifndef AWS_IO_APP_CHANNEL_HANDLER_H
#define AWS_IO_APP_CHANNEL_HANDLER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/io.h>
#include <aws/io/tls_channel_handler.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_channel;
struct aws_channel_handler;
struct aws_channel_slot;
struct aws_byte_buf;
struct aws_tls_connection_options;

typedef void(aws_app_client_handler_on_read_fn)(
    struct aws_channel_handler *handler,
    struct aws_byte_buf *buffer,
    void *user_data);

AWS_EXTERN_C_BEGIN

AWS_IO_API struct aws_channel_handler *aws_app_client_handler_new(
    struct aws_allocator *allocator,
    struct aws_channel *channel,
    aws_app_client_handler_on_read_fn *on_read,
    void *user_data);

AWS_IO_API void aws_app_client_handler_write(
    struct aws_channel_handler *handler,
    struct aws_byte_buf *buffer,
    aws_channel_on_message_write_completed_fn *on_completed,
    void *user_data);

AWS_IO_API int aws_app_client_handler_tls_upgrade(
    struct aws_allocator *allocator,
    struct aws_channel *channel,
    const char *host_name,
    aws_tls_on_negotiation_result_fn *on_negotiation_result,
    void *user_data);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_APP_CHANNEL_HANDLER_H */
