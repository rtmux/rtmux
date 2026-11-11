/* Planted fixture: a call site to a lower-rank node (utils). */
int f(int fd)
{
    /* ipc-send: romfs -> utils */
    return msg_send(fd, 0, 0, 0, 0);
}
