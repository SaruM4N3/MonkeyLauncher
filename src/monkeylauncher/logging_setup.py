import logging
import os
import sys
from logging.handlers import RotatingFileHandler

from .config import LOG_DIR, LOG_FILE

class ColorFormatter(logging.Formatter):
    """Colorizes console output by level; plain text when writing to a file."""
    COLORS = {
        logging.DEBUG:    '\033[2;37m',   # dim gray
        logging.INFO:     '\033[0;36m',   # cyan
        logging.WARNING:  '\033[1;33m',   # yellow
        logging.ERROR:    '\033[0;31m',   # red
        logging.CRITICAL: '\033[1;41;37m',  # bold white on red
    }
    RESET = '\033[0m'

    def __init__(self, use_color):
        super().__init__(fmt='%(asctime)s %(levelname)-8s %(message)s',
                          datefmt='%H:%M:%S')
        self.use_color = use_color

    def format(self, record):
        text = super().format(record)
        if self.use_color:
            color = self.COLORS.get(record.levelno, '')
            return f'{color}{text}{self.RESET}'
        return text

def setup_logging():
    debug = (os.environ.get('MONKEYLAUNCHER_DEBUG') == '1'
              or '--debug' in sys.argv or '-v' in sys.argv)

    logger = logging.getLogger('monkeylauncher')
    logger.setLevel(logging.DEBUG)

    console = logging.StreamHandler(sys.stderr)
    console.setLevel(logging.DEBUG if debug else logging.INFO)
    console.setFormatter(ColorFormatter(use_color=sys.stderr.isatty()))
    logger.addHandler(console)

    try:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        file_handler = RotatingFileHandler(
            LOG_FILE, maxBytes=2_000_000, backupCount=2, encoding='utf-8')
        file_handler.setLevel(logging.DEBUG)
        file_handler.setFormatter(ColorFormatter(use_color=False))
        logger.addHandler(file_handler)
    except OSError as e:
        logger.warning(f"Could not open log file {LOG_FILE}: {e}")

    return logger

log = setup_logging()
