# Manual integration test plan

- [ ] Start the server on port 8080.
- [ ] Connect client A and client B.
- [ ] Send a room message from A; verify B receives it.
- [ ] Change A's nickname using `/nick Alice`.
- [ ] Join a second room from B using `/join dev`.
- [ ] Verify room isolation by sending messages from both rooms.
- [ ] Send a private message using `/msg Alice hello`.
- [ ] Verify `/who` lists only users in the current room.
- [ ] Verify `/rooms` lists active rooms.
- [ ] Disconnect a client and verify remaining clients receive the leave notice.
- [ ] Stop the server with Ctrl-C and verify clean shutdown.
- [ ] Rebuild with AddressSanitizer and UndefinedBehaviorSanitizer.
