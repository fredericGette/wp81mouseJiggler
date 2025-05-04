# wp81mouseJiggler
Turns your _Lumia 520_ into a kind of _Microsoft Bluetooth Mobile Mouse 3600_.  
There is no graphical interface as this is a Proof-of-Concept before another more polished version.  

Example of result with a Chromebook:   
![video capture ChromeOS](lumiaMouse520_chromeos.gif)  
(animated gif optimized with [gifsicle](https://github.com/kohler/gifsicle))  

## Description

At the beginning of the programm, the phone is in advertising mode. 
This advertising stops as soon as a connection is established with another device.  
- When the devices were not paired previously, then the phone receives a _Pairing Request_ and a [LE Legacy Pairing](#le-legacy-pairing) starts. Usually, this pairing is initiated by a user.   
- When the devices were paired previously, the phone directly receives a _LTK Request_ and the encryption of the connection starts. Usually, this connection doesn't require any user interaction. The _LTK Request_ contains the information given by the _Master Identification_ message during the previous pairing and the LTK(Long Term Key) is the one of the _Encryption Information_ message.  
  
At the end of the both cases, the connection is encrypted and the phone is ready to respond to the ATTributes requests. Then it starts to send the [notifications](#notification).   

<a name="le-legacy-pairing">LE Legacy Pairing:</a>
| | mouse | | computer | |
|:-:|-:|:-:|:-|:-|
|1| | <- | Pairing Request | |
|2| Pairing Response | -> | | |
|3| | <- | Pairing Confirm | _Computed with a random value and information coming from the Pairing Request/Response and from the connection_ |
|4| Pairing Confirm | -> | | |
|5| | <- | Pairing Random | _Random value used to compute the Pairing Confirm_ |
|6| Pairing Random | -> | | |
|7| | <- | LTK Request | _The key of the request is 0x0000000000. In fact the computer requests the STK(Short Term Key)_ |
|8| LTK Request Reply | -> | | _Responding device must compute the same STK than the Initiating device_ |
|9| Encryption Information | -> | | _The real LTK_ |
|10| Master Identification | -> | | _A key to store the LTK_ |
|11| | <- | Signing Information | _Can be ignored_ |

The phone is acting like a mouse, and it is:  
- The Bluetooth Low Energy **Slave**
- The **Responding** device during the pairing process
- The Attribute **Server**

The computer (or any other device) is:  
- The Bluetooth Low Energy **Master**
- The **Initiator** device during the pairing process
- The Attribute **Client**

<a name="notification">Notification:</a>

>[!NOTE]
>Some Attribute Clients (like ChromeOS) require a Maximum Transmission Unit (MTU) of 23 bytes.  
>In the case of the Lumia 520, it looks like the number of messages lost increases when the Connection Interval is above 7.5ms (this is the minimum Connection Interval authorized).
>Sometimes the notifications send by the phone are stalling. It looks like this is caused by a MIC(Message Integrity Check) error. When this problem is detected, the program automatically reset the connection.

>[!WARNING]
>For an unkown reason you have to reboot the phone when no connection is established during the advertising phase. Otherwise no further connection can be established during the next advertising phase.

Tested with the following configurations:
- Ubuntu 23.10 (Dell Latitude E5470)
- ChromeOS 134.0 (ASUS CX5601)
- Windows 10 Pro (Dell Latitude E5470)
- Windows 8.1 Pro (Dell Latitude E5470)
- Android 10 (Google Pixel)
- Android 15 (Google Pixel 7a)

