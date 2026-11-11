# Memory Safety Inventory Negative Fixture

| boundary | trusted input | maximum length | ownership or lifetime rule | cleanup path | test_command |
| --- | --- | --- | --- | --- | --- |
| missing-test-command | Untrusted bytes. | Fixed. | No retained pointer. | Reject. |  |
