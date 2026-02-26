import argparse
import numpy as np
import socket
import time

class FrameProducerError(Exception):

    def __init__(self, msg):
        self.msg = msg

    def __str__(self):
        return repr(self.msg)

class FrameProducer(object):

    PADDING_WORDS = 6
    HeaderType = np.dtype([
    ("frame_number", '<u8'),
    ("padding", "<u8", (PADDING_WORDS,)),
    ("packet_number", ">u4"),
    ("markers", "u1"),
    ("_ununsed_1", "u1"),
    ("padding_len", "u1"),
    ("readout_lane", "u1"),
])

    SOF = 0x80
    EOF = 0x40
    SOP = 0x20
    EOP = 0x10

    DEFAULT_PAYLOAD_LENGTH = 8000

    def __init__(self):

        # Define default list of destination IP address(es) with port(s)
        defaultDestAddr = ['10.0.100.6:1234']

        parser = argparse.ArgumentParser(prog="frame_producer", description="FrameProducer - generate a simulated UDP frame data stream")

        parser.add_argument('--destaddr', nargs='*', # nargs: 1 flag accept multiple arguments
                            help="list destination host(s) IP address:port (e.g. 0.0.0.1:8081)")
        parser.add_argument('--frames', '-n', type=int, default=1,
                            help='select number of frames to transmit')
        parser.add_argument('--start_frame', '-s', type=int, default=0,
                            help='select starting frame number')
        parser.add_argument('--interval', '-t', type=float, default=0.1,
                            help="select frame interval in seconds")

        args = parser.parse_args()

        if args.destaddr == None:
            args.destaddr = defaultDestAddr

        self.destaddr = args.destaddr
        self.frames   = args.frames
        self.start    = args.start_frame
        self.interval = args.interval

        self.num_rows = 1000
        self.num_cols = 1000
        self.num_pixels = self.num_rows * self.num_cols

        self.data = np.arange(self.num_pixels, dtype=np.uint16)
        self.data_stream = self.data.tobytes()

        self.payload_length = self.DEFAULT_PAYLOAD_LENGTH

    def run(self):

        (self.host, self.port) = ([], [])
        for index in self.destaddr:
            (host, port) = index.split(':')
            self.host.append(host)
            self.port.append(int(port))

        print("Starting frame transmission to:", self.host)

        # Open UDP socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Create packet header
        header = np.zeros(1, dtype=self.HeaderType)
        header["markers"] = 0
        header["padding_len"] = self.PADDING_WORDS * 8

        total_bytes_sent = 0
        total_packets_sent = 0

        run_start_time = time.time()

        # Number of destination(s)
        index = len(self.host)

        for frame in range(self.start, self.start + self.frames):

            frame_start_time = time.time()

            # Construct host & port from lists
            (host, port) = (self.host[frame % index], self.port[frame % index] )

            bytes_remaining = len(self.data_stream)
            bytes_sent = 0
            stream_pos = 0
            packet_number = 0

            header["frame_number"] = frame

            while bytes_remaining > 0:

                header["packet_number"] = packet_number
                header["markers"] = 0

                if packet_number == 0:
                    header["markers"] |= self.SOF

                if bytes_remaining <= self.payload_length:
                    send_size = bytes_remaining
                    header["markers"] |= self.EOF
                else:
                    send_size = self.payload_length

                packet = header.tobytes() + self.data_stream[stream_pos:stream_pos+send_size]

                bytes_sent += sock.sendto(packet, (host, port))

                bytes_remaining -= send_size
                stream_pos += send_size
                packet_number += 1

            print(f"Frame {frame} sent {bytes_sent} bytes in {packet_number} packets")

            total_bytes_sent += bytes_sent
            total_packets_sent += packet_number

            frame_end_time = time.time()
            wait_time = (frame_start_time + self.interval) - frame_end_time
            if wait_time > 0:
                time.sleep(wait_time)

        run_time = time.time() - run_start_time

        # Close socket
        sock.close()

        print(
            f"Sent {self.frames} frames in {total_packets_sent} "
            f"packets {total_bytes_sent} bytes in {run_time:.3f} s"
        )

def main():

    producer = FrameProducer()
    producer.run()

if __name__ == '__main__':
    main()