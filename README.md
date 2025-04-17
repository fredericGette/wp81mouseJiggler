# wp81mouseJiggler
Turn your _Lumia 520_ into a kind of _Microsoft Bluetooth Mobile Mouse 3600_.

Example with a Chromebook:   
![video capture ChromeOS](lumiaMouse520_chromeos.gif)  
(animated gif optimized with [gifsicle](https://github.com/kohler/gifsicle))  

Tested with the following configurations:
- Ubuntu 23.10 (Dell Latitude E5470)
- ChromeOS 134.0 (ASUS CX5601)
- Windows 10 Pro (Dell Latitude E5470)

LE Legacy Pairing:  
| | mouse | | computer | |
|:-:|-:|:-:|:-|:-|
|1| | <- | Pairing Request | |
|2| Pairing Response | -> | | |
|3| | <- | Pairing Confirm | _Computed with a random value and information coming from the Pairing Request/Response_ |
|4| Pairing Confirm | -> | | |
|5| | <- | Pairing Random | _Random value used to compute the Pairing Confirm_ |
|6| Pairing Random | -> | | |
|7| | <- | LTK Request | _In fact it's the STK_ |
|8| LTK Request Reply | -> | | _Responding device must compute the same STK than the Initiating device_ |
|9| Encryption Information | -> | | _The real LTK_ |
|10| Master Identification | -> | | _A key to store the LTK_ |
|11| | <- | Signing Information | _Can be ignored_ |

The mouse is:  
- The Bluetooth Low Energy **Slave**
- The **Responding** device during the pairing process
- The Attribute **Server**

The computer is:  
- The Bluetooth Low Energy **Master**
- The **Initiator** device during the pairing process
- The Attribute **Client**
