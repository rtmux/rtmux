/* Planted fixture: a call site with no ipc-send annotation. */
int f(int fd)
{
    return msg_send(fd, 0, 0, 0, 0);
}
