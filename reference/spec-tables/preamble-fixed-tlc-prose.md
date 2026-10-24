# MIL-STD-188-110D Appendix D - preamble Fixed/TLC/count prose (doc pages 165-167, PDF pages 170-172)

Transcribed by eye from the page images (independent transcriber A). Prose is
paraphrase-free where quoted; structure summarised where noted.

## D.5.2.1 Synchronization preamble (doc 165)

The synchronization preamble consists of two main sections: a transmitter level
control (TLC) settling time section, and a synchronization section containing a
repeated preamble super-frame. The preamble super-frame consists of three
distinct subsections: one with a fixed (known) modulation, one to convey a
downcount, and one to convey waveform identification. The superframe shall be
repeated M times. The Synchronization section shall be immediately followed by
the modulated data (Figure D-8).

Figure D-8 layout: [TLC section][Super-frame][Super-frame]...[Data Modulation];
each super-frame = [Fixed][4 Downcount symbols c3, c2, c1, c0][5 WID symbols
w4, w3, w2, w1, w0].

## D.5.2.1.1 4-ary Orthogonal Walsh Modulation in the preamble (doc 165-166)

4-ary orthogonal Walsh modulation shall be used in the synchronization section
of the preamble. The length of each channel symbol, in chips or symbols,
depends on the bandwidth and is given by Table D-XIII.

(doc 166:) The 4-ary orthogonal Walsh modulation shall use the Walsh sequences
in Table D-XIV. The di-bit representing the 2 bits of information to convey is
mapped into a 4 element Walsh sequence of 0 and 4 as defined in Table D-XIV.
This 4-element sequence shall be repeated to satisfy the Walsh sequence length
requirement in Table D-XIII for the bandwidth in use.

Scrambling shall be performed by aligning the expanded Walsh sequence with the
Fixed, Count, or the Waveform ID synchronization preambles defined in Tables
D-XVIII, D-XIX, D-XX, respectively, and performing a modulo 8 addition between
the specified 8-PSK symbol from the table and the corresponding Walsh element.

## D.5.2.1.2 TLC Section (doc 166)

The first section of the preamble, denoted TLC, is provided exclusively for
radio and modem TGC and AGC. It shall consist of N blocks of 8-PSK. The length
of each block in PSK symbols, based on the bandwidth used in the transmission
to follow, shall be as shown in Table D-XIII. The value of N shall be
configurable to range from 0 to 255 (for N=0 this first section is not
transmitted). **These symbols shall be formed by taking the complex conjugate
of the symbols of the sequence specified below for the Fixed section in Table
D-XVIII.**

## D.5.2.1.3 Synchronization Section (doc 166)

The Fixed subsection of the super-frame shall consist of either 1 or 9
orthogonal Walsh modulated channel symbols. The length of each channel symbol,
dependent on bandwidth, is given in Table D-XIII. **For the case of the single
Walsh symbol the di-bit shall be 3 (binary 11), and the super frame shall be
transmitted only once (M=1).** **For the case of 9 Walsh symbols the di-bits
shall be {0, 0, 2, 1, 2, 1, 0, 2, 3}, 3 being the last di-bit transmitted.**
The Fixed subsection is intended exclusively for synchronization and Doppler
offset removal purposes.

The next subsection shall consist of four orthogonal Walsh modulated di-bits,
labeled as c3, c2, c1 and c0, each conveying two bits of information. This
subsection represents a 5 bit downcount plus 3 parity bits. This count shall be
initialized to a value of (M-1) and shall be decremented with each of the M
preamble repetitions until it reaches zero in the final super-frame before data
begins.

The final subsection of the preamble super-frame shall consist of five
orthogonal Walsh modulated channel symbols, each conveying two bits of
information. These di-bits are labeled as w4, w3, w2, w1, and w0. These 10 bits
represent a Waveform ID consisting of waveform number, interleaver option,
convolutional code length and parity check.

## D.5.2.1.3.1 Mapping of the downcount di-bits c3, c2, c1, and c0 (doc 167)

The 5 bit super-frame down count is initialized to M-1 where M is the number of
repeats of the super-frame and can be viewed as a binary number b4b3b2b1b0
where b4 is the MSB and b0 is the LSB. Bits b7, b6, and b5 shall contain a
parity check computed over b4b3b2b1b0 as follows (^ = exclusive-or):

    b7 = b1 ^ b2 ^ b3
    b6 = b2 ^ b3 ^ b4
    b5 = b0 ^ b1 ^ b2

C3 shall contain the two MSBs b7 and b6, b7 being the MSB. C2 shall contain the
next two bits of the count b5 and b4, b5 being the MSB. C1 shall contain the
next two bits of the count b3 and b2, b3 being the MSB. C0 shall contain the
last two bits of the count b1 and b0, b1 being the MSB.

## D.5.2.1.3.2 Mapping of the Waveform ID di-bits w4, w3, w2, w1, and w0 (doc 167)

The 10 bit waveform ID field consists of 5 di-bits w4, w3, w2, w1, and w0. The
10 bits are labeled d9 down to d0. W4 contains d9 and d8, where d9 is the MSB,
w3 contains d7 and d6, and so on down to w0 which contains d1 and d0.

The 3 LSBs, d2, d1, and d0, shall contain a 3 bit checksum calculated over
d9d8d7d6d5d4d3 as follows (^ = exclusive-or):

    d2 = d9 ^ d8 ^ d7
    d1 = d7 ^ d6 ^ d5
    d0 = d5 ^ d4 ^ d3
