class Account:
    def __init__(self, owner, balance=0):
        self.owner = owner
        self.balance = balance

    def deposit(self, amount):
        self._validate(amount)
        self.balance += amount

    def _validate(self, amount):
        if amount <= 0:
            raise ValueError("amount must be positive")
