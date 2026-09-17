#include "ui_snapshot.h"

static hh_status_t  s_status;
static hh_message_t s_messages[HH_MESSAGES];

const hh_status_t *ui_status(void)
{
    hh_service_status(&s_status);
    return &s_status;
}

const hh_message_t *ui_messages(size_t *count)
{
    *count = hh_service_messages(s_messages, HH_MESSAGES);
    return s_messages;
}
