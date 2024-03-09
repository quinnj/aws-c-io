/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/error.h>
#include <aws/common/task_scheduler.h>
#include <aws/io/app_channel_handler.h>
#include <aws/io/channel.h>
#include <aws/io/event_loop.h>
#include <aws/io/logging.h>

struct app_client_handler {
    struct aws_channel_slot *slot;
    aws_app_client_handler_on_read_fn *on_read;
    void *user_data;
};

static int s_app_client_handler_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)slot;
    AWS_LOGF_INFO(
        AWS_LS_IO_APP_HANDLER,
        "id=%p: process_read_message called on app handler with message %p",
        (void *)handler,
        (void *)message);
    AWS_ASSERT(message);
    struct app_client_handler *app_client_handler = handler->impl;
    AWS_ASSERT(app_client_handler->on_read);
    app_client_handler->on_read(handler, &message->message_data, app_client_handler->user_data);
    aws_mem_release(message->allocator, message);
    return AWS_OP_SUCCESS;
}

static int s_app_client_handler_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)slot;
    (void)message;

    AWS_LOGF_FATAL(
        AWS_LS_IO_APP_HANDLER,
        "id=%p: process_write_message called on "
        "app handler. This should never happen",
        (void *)handler);

    /*since an app handler will ALWAYS be the last handler in a channel,
     * this should NEVER happen, if it does it's a programmer error.*/
    AWS_ASSERT(0);
    return aws_raise_error(AWS_IO_CHANNEL_ERROR_ERROR_CANT_ACCEPT_INPUT);
}

static int s_app_client_handler_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size) {
    (void)handler;
    aws_channel_slot_increment_read_window(slot, size);
    return AWS_OP_SUCCESS;
}

static int s_app_client_handler_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool abort_immediately) {
    AWS_LOGF_INFO(
        AWS_LS_IO_APP_HANDLER, "id=%p: shutdown called on app handler with error code %d", (void *)handler, error_code);
    return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, abort_immediately);
}

static void s_app_client_handler_destroy(struct aws_channel_handler *handler) {
    struct app_client_handler *app_client_handler = handler->impl;
    aws_mem_release(handler->alloc, app_client_handler);
    aws_mem_release(handler->alloc, handler);
}

static size_t s_app_client_handler_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;
    return 0;
}

static size_t s_app_client_handler_initial_window_size(struct aws_channel_handler *handler) {
    (void)handler;
    return SIZE_MAX;
}

static struct aws_channel_handler_vtable s_app_client_handler_vtable = {
    .process_read_message = s_app_client_handler_process_read_message,
    .process_write_message = s_app_client_handler_process_write_message,
    .increment_read_window = s_app_client_handler_increment_read_window,
    .shutdown = s_app_client_handler_shutdown,
    .destroy = s_app_client_handler_destroy,
    .message_overhead = s_app_client_handler_message_overhead,
    .initial_window_size = s_app_client_handler_initial_window_size,
};

struct aws_channel_handler *aws_app_client_handler_new(
    struct aws_allocator *allocator,
    struct aws_channel *channel,
    aws_app_client_handler_on_read_fn *on_read,
    void *user_data) {
    AWS_LOGF_INFO(
        AWS_LS_IO_APP_HANDLER,
        "id=%p: Creating app client handler with on_read callback %p",
        (void *)channel,
        (void *)on_read);
    struct aws_channel_handler *handler = aws_mem_acquire(allocator, sizeof(struct aws_channel_handler));
    if (!handler) {
        return NULL;
    }

    struct app_client_handler *app_client_handler = aws_mem_acquire(allocator, sizeof(struct app_client_handler));
    if (!app_client_handler) {
        aws_mem_release(allocator, handler);
        return NULL;
    }

    AWS_ZERO_STRUCT(*handler);
    AWS_ZERO_STRUCT(*app_client_handler);

    handler->vtable = &s_app_client_handler_vtable;
    handler->alloc = allocator;
    handler->impl = app_client_handler;

    app_client_handler->on_read = on_read;
    app_client_handler->user_data = user_data;

    // set up app channel slot
    AWS_LOGF_INFO(AWS_LS_IO_APP_HANDLER, "id=%p: Creating app client handler slot", (void *)channel);
    app_client_handler->slot = aws_channel_slot_new(channel);
    if (!app_client_handler->slot) {
        aws_mem_release(allocator, app_client_handler);
        aws_mem_release(allocator, handler);
        return NULL;
    }
    AWS_LOGF_INFO(AWS_LS_IO_APP_HANDLER, "id=%p: inserting new app handler slot into channel", (void *)channel);
    if (aws_channel_slot_insert_end(channel, app_client_handler->slot)) {
        aws_mem_release(allocator, app_client_handler);
        aws_mem_release(allocator, handler);
        return NULL;
    }
    AWS_LOGF_INFO(AWS_LS_IO_APP_HANDLER, "id=%p: setting handler for app client handler slot", (void *)channel);
    if (aws_channel_slot_set_handler(app_client_handler->slot, handler)) {
        aws_mem_release(allocator, app_client_handler);
        aws_mem_release(allocator, handler);
        return NULL;
    }
    return handler;
}

struct app_handler_write_task_args {
    struct aws_channel_handler *handler;
    struct aws_channel_slot *slot;
    struct aws_byte_buf *buffer;
    aws_channel_on_message_write_completed_fn *on_completed;
    void *user_data;
    struct aws_channel_task task;
};

static int s_app_client_handler_write(struct aws_channel_slot *slot, struct aws_byte_buf *buffer) {
    ssize_t bytes_written = 0;
    ssize_t len = buffer->len;
    AWS_LOGF_INFO(AWS_LS_IO_APP_HANDLER, "id=%p: writing %d bytes to app handler", (void *)slot->channel, len);
    while (bytes_written < len) {
        struct aws_io_message *msg =
            aws_channel_acquire_message_from_pool(slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, len - bytes_written);
        if (!msg) {
            return AWS_OP_ERR;
        }
        ssize_t cap = msg->message_data.capacity;
        struct aws_byte_cursor write_buffer = aws_byte_cursor_from_array(buffer->buffer + bytes_written, cap);
        aws_byte_buf_append(&msg->message_data, &write_buffer);
        AWS_LOGF_INFO(
            AWS_LS_IO_APP_HANDLER, "id=%p: sending %d byte message in app handler", (void *)slot->channel, cap);
        if (aws_channel_slot_send_message(slot, msg, AWS_CHANNEL_DIR_WRITE)) {
            aws_mem_release(msg->allocator, msg);
            return AWS_OP_ERR;
        }
        bytes_written += cap;
    }
    return AWS_OP_SUCCESS;
}

static void s_app_handler_write_task(struct aws_channel_task *task, void *arg, enum aws_task_status task_status) {
    (void)task;
    if (task_status == AWS_TASK_STATUS_RUN_READY) {
        struct app_handler_write_task_args *write_task_args = arg;
        write_task_args->on_completed(
            write_task_args->slot->channel,
            NULL,
            s_app_client_handler_write(write_task_args->slot, write_task_args->buffer),
            write_task_args->user_data);
    }
}

void aws_app_client_handler_write(
    struct aws_channel_handler *handler,
    struct aws_byte_buf *buffer,
    aws_channel_on_message_write_completed_fn *on_completed,
    void *user_data) {
    AWS_ASSERT(handler);
    struct aws_channel_slot *slot = handler->slot;
    AWS_ASSERT(slot);
    AWS_ASSERT(buffer);
    AWS_ASSERT(on_completed);
    AWS_LOGF_INFO(
        AWS_LS_IO_APP_HANDLER,
        "id=%p: app client handler write called with buffer len %d",
        (void *)slot->channel,
        buffer->len);
    if (aws_channel_thread_is_callers_thread(slot->channel)) {
        on_completed(slot->channel, NULL, s_app_client_handler_write(slot, buffer), user_data);
    } else {
        struct app_handler_write_task_args *write_task_args =
            aws_mem_acquire(handler->alloc, sizeof(struct app_handler_write_task_args));
        write_task_args->handler = handler;
        write_task_args->buffer = buffer;
        write_task_args->slot = slot;
        write_task_args->on_completed = on_completed;
        write_task_args->user_data = user_data;
        aws_channel_task_init(&write_task_args->task, s_app_handler_write_task, write_task_args, "app_handler_write");
        aws_channel_schedule_task_now(slot->channel, &write_task_args->task);
    }
}

int aws_app_client_handler_tls_upgrade(
    struct aws_allocator *allocator,
    struct aws_channel *channel,
    const char *host_name,
    aws_tls_on_negotiation_result_fn *on_negotiation_result,
    void *user_data) {
    AWS_ASSERT(channel);
    AWS_ASSERT(host_name);
    AWS_ASSERT(on_negotiation_result);
    AWS_LOGF_INFO(AWS_LS_IO_APP_HANDLER, "id=%p: app client handler tls upgrade called", (void *)channel);
    struct aws_channel_slot *slot = aws_channel_get_first_slot(channel);
    AWS_ASSERT(slot);
    struct aws_tls_ctx *tls_ctx = NULL;
    struct aws_tls_ctx_options tls_ctx_options;
    AWS_ZERO_STRUCT(tls_ctx_options);
    struct aws_tls_connection_options tls_connection_options;
    AWS_ZERO_STRUCT(tls_connection_options);
    aws_tls_ctx_options_init_default_client(&tls_ctx_options, allocator);
    tls_ctx = aws_tls_client_ctx_new(allocator, &tls_ctx_options);
    aws_tls_connection_options_init_from_ctx(&tls_connection_options, tls_ctx);
    struct aws_byte_cursor host_name_cursor = aws_byte_cursor_from_c_str(host_name);
    aws_tls_connection_options_set_server_name(&tls_connection_options, allocator, &host_name_cursor);
    aws_tls_connection_options_set_callbacks(&tls_connection_options, on_negotiation_result, NULL, NULL, user_data);
    if (aws_channel_setup_client_tls(slot, &tls_connection_options)) {
        return AWS_OP_ERR;
    }
    aws_tls_connection_options_clean_up(&tls_connection_options);
    aws_tls_ctx_options_clean_up(&tls_ctx_options);
    aws_tls_ctx_release(tls_ctx);
    return AWS_OP_SUCCESS;
}
