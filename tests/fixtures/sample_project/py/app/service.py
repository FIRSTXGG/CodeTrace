from .models import Account
from . import utils


def open_account(owner):
    acc = Account(owner)
    acc.deposit(utils.initial_bonus())
    return acc
