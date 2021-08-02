const REPEAT_MARKER = [0x69, 0x72];

module.exports = class LinuxDeviceUtils {
  /**
   * Decodes the custom buffer that includes the interval and repetitions
   *
   * @returns {*}
   * @param chunk
   */
  static decodeWriteBuffer(chunk) {
    // Custom parsing for IR header.
    // The stream.write function only accepts an buffer, therefore we need to encode the interval and repetitions in the buffer before using write().
    let repetitions = 0;
    let interval = 0;
    if (chunk[0] === REPEAT_MARKER[0] && chunk[1] === REPEAT_MARKER[1]
      && chunk[5] === REPEAT_MARKER[0] && chunk[6] === REPEAT_MARKER[1]) {
      repetitions = chunk[2];
      interval = Buffer.from(chunk).readUInt16BE(3);
      chunk = chunk.slice(7);
    }

    return {
      buffer: chunk,
      repetitions,
      interval,
    };
  }

  /**
   * Encodes the buffer, interval and repetitions to be send to the .write
   *
   * @param buffer
   * @param repetitions
   * @param interval
   * @returns {*}
   */
  static encodeWriteBuffer({ buffer, repetitions = 0, interval = 0 }) {
    // Custom parsing for IR header.
    // the .write() function only accepts an buffer, therefore we need to encode the interval and repetitions in the buffer before using write().
    const irHeaderBuffer = Buffer.alloc(7);
    irHeaderBuffer.writeUInt8(REPEAT_MARKER[0]);
    irHeaderBuffer.writeUInt8(REPEAT_MARKER[1], 1);
    irHeaderBuffer.writeUInt8(repetitions, 2); // max repetitions = 255
    irHeaderBuffer.writeUInt16BE(interval, 3); // max interval = 65,535us
    irHeaderBuffer.writeUInt8(REPEAT_MARKER[0], 5);
    irHeaderBuffer.writeUInt8(REPEAT_MARKER[1], 6);
    return Buffer.concat([irHeaderBuffer, buffer]);
  }
};
