# Vision

It started with a simple observation: cloud gaming has come a long way. The fact that you can boot up a game from inside a web browser is genuinely impressive. But there's a catch — the input lag. Every button press has to travel across the network, get processed, and finally reach the console. That waiting adds up.

I wanted to make Switch cloud streaming a thing. The video part? Surprisingly easy — a capture card and something like Parsec or Sunshine handles that. The hard part is the other direction: getting inputs from a computer to a Switch in a way the Switch actually understands.

My goal was straightforward: build something that sends input data as fast as physically possible. Nothing fancy, just raw speed.

I started researching and built support for Hori controllers first, assuming they were the only ones that supported multiple controllers over a single USB connection. Then I wanted to add gyro and rumble — features typically locked to official Pro controllers.

During testing, I discovered something I wasn't expecting: it's possible to connect **multiple** Pro controllers through a **single USB cable**. Not just one — several, simultaneously, with full gyro and rumble support.

That discovery changed the scope of the project entirely.

Now I can finally play co-op games with my friends who don't own the same console I do — all with a cheap capture card that handles 1080p 60, a Raspberry Pi, and a program like Parsec or Sunshine for streaming the video feed.

